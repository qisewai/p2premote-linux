#pragma once
class QApplication;
namespace p2premote::linux_host {
int RunHostClipboardSession(QApplication& app, int fd);
}
