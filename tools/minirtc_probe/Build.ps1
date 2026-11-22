param([string]$Xmake, [string]$Perl)

$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$deps=(New-Item -ItemType Directory -Force -Path (Join-Path $root 'out\minirtc-deps')).FullName
$source=Join-Path $deps 'minirtc-f5b53e9'
$revision='f5b53e920db0713b456e23af6a0fe9907d94a612'
if (!$Xmake) {
    $portable=Join-Path $deps 'xmake-3.1.1\xmake\xmake.exe'
    $Xmake=if (Test-Path -LiteralPath $portable) {$portable} else {(Get-Command xmake.exe).Source}
}
$Xmake=(Resolve-Path -LiteralPath $Xmake).Path
if ($Perl) {
    $perlDir=Split-Path -Parent (Resolve-Path -LiteralPath $Perl).Path
    $env:PATH="$perlDir;$env:PATH"
    $env:P2PREMOTE_MINIRTC_SYSTEM_PERL='1'
}
if (!(Test-Path -LiteralPath $source)) {
    git init --quiet $source
    if ($LASTEXITCODE) {exit $LASTEXITCODE}
}
$head=git -C $source rev-parse --verify HEAD 2>$null
if ($LASTEXITCODE) {
    git -C $source fetch --depth 1 https://github.com/kunkundi/minirtc.git $revision
    if ($LASTEXITCODE) {exit $LASTEXITCODE}
    git -C $source checkout --detach FETCH_HEAD
    if ($LASTEXITCODE) {exit $LASTEXITCODE}
} elseif ($head -ne $revision) {
    throw "Expected MiniRTC $revision in $source"
}

function Get-UpstreamText([string]$file) {
    $lines=git -C $source show "$($revision):$file"
    if ($LASTEXITCODE) {throw "Cannot read pinned MiniRTC file $file"}
    return ($lines -join "`n") + "`n"
}

$recipe=Get-UpstreamText 'xmake.lua'
$recipe=$recipe.Replace('    add_requireconfs("libyuv", {configs = {toolchains = "clang-cl"}})', '    -- Data-only probe uses MSVC for libyuv.')
$recipe += @'

target_end()
if os.getenv("P2PREMOTE_MINIRTC_SYSTEM_PERL") == "1" then
    add_requireconfs("**.strawberry-perl", {system = true, override = true})
end
includes(path.join(os.getenv("P2PREMOTE_MINIRTC_PROBE_DIR"), "target.lua"))
'@
[IO.File]::WriteAllText((Join-Path $source 'xmake.lua'),$recipe)

# Keep the test certificate in this process's OpenSSL store and require the
# normal chain and hostname/IP checks. No Windows certificate store changes.
$ws=Get-UpstreamText 'src/ws/ws_client.cpp'
$originalInclude="#if defined(__ANDROID__)`n#include <asio/ssl/host_name_verification.hpp>`n#endif"
$ws=$ws.Replace($originalInclude,"#include <asio/ssl/host_name_verification.hpp>`n#include <windows.h>")
$ws=$ws.Replace('    LoadTlsSystemRootCertificates(ctx->native_handle());',@'
    char probe_ca[32768]{};
    const auto probe_ca_size = GetEnvironmentVariableA("P2PREMOTE_MINIRTC_PROBE_CA", probe_ca, sizeof(probe_ca));
    if (probe_ca_size == 0 || probe_ca_size >= sizeof(probe_ca) ||
        SSL_CTX_load_verify_locations(ctx->native_handle(), probe_ca, nullptr) != 1) {
      throw std::runtime_error("cannot load the probe CA certificate");
    }
'@.TrimEnd())
$ws=$ws.Replace('            return self->OnTlsVerify(preverified, ctx);',@'
            return websocketpp::lib::asio::ssl::host_name_verification(
                websocketpp::uri(self->uri_).get_host())(preverified, ctx);
'@.TrimEnd())
if (!$ws.Contains('cannot load the probe CA certificate') -or !$ws.Contains('websocketpp::uri(self->uri_)')) {
    throw 'The probe TLS adaptation did not match the pinned MiniRTC source.'
}
[IO.File]::WriteAllText((Join-Path $source 'src\ws\ws_client.cpp'),$ws)

$env:P2PREMOTE_MINIRTC_PROBE_DIR=$PSScriptRoot
Push-Location -LiteralPath $source
try {
    & $Xmake f -p windows -a x64 -m release --USE_CUDA=false -y
    if ($LASTEXITCODE) {exit $LASTEXITCODE}
    & $Xmake b -y p2premote_minirtc_probe
    if ($LASTEXITCODE) {exit $LASTEXITCODE}
} finally {Pop-Location}
Get-Item -LiteralPath (Join-Path $root 'out\minirtc-probe\p2premote_minirtc_probe.exe')
