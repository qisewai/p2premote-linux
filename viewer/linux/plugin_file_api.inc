    static int FsRoots(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        lua_createtable(state, 1, 0);
        lua_createtable(state, 0, 3);
        lua_pushliteral(state, "/");
        lua_setfield(state, -2, "path");
        lua_pushliteral(state, "/");
        lua_setfield(state, -2, "name");
        lua_pushliteral(state, "drive");
        lua_setfield(state, -2, "kind");
        lua_rawseti(state, -2, 1);
        return 1;
    }

    static int FsHome(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        lua_createtable(state, 0, 3);
        for (const auto& [key, location] : {
            std::pair{"home", QStandardPaths::HomeLocation},
            std::pair{"desktop", QStandardPaths::DesktopLocation},
            std::pair{"downloads", QStandardPaths::DownloadLocation}}) {
            const auto path = QStandardPaths::writableLocation(location).toStdString();
            if (!path.empty()) {
                PushString(state, path);
                lua_setfield(state, -2, key);
            }
        }
        return 1;
    }

    static int FsJoin(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        const auto directory = SafeFsPath(CheckString(state, 1));
        const auto name = CheckString(state, 2);
        if (!directory || !ValidLeafName(name) || !Utf8ToWide(name)) return FsError(state, "路径或文件名无效");
        PushString(state, (*directory / std::filesystem::path(name)).native());
        return 1;
    }

    static lua_Integer FileStamp(const struct stat& info) {
        return static_cast<lua_Integer>(info.st_mtim.tv_sec) * 1'000'000'000 + info.st_mtim.tv_nsec;
    }

    static int FsStat(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path) return FsError(state, "路径无效");
        const DirectoryHandle parent(path->has_relative_path() ? path->parent_path() : *path);
        if (parent.get() < 0) return FsError(state, "无法访问目录");
        struct stat info{};
        const bool exists = ::fstatat(parent.get(), path->has_relative_path() ? path->filename().c_str() : ".",
            &info, AT_SYMLINK_NOFOLLOW) == 0;
        if (!exists && errno != ENOENT) return FsError(state, "无法读取文件信息");
        lua_createtable(state, 0, 6);
        lua_pushboolean(state, exists);
        lua_setfield(state, -2, "exists");
        lua_pushboolean(state, S_ISDIR(info.st_mode));
        lua_setfield(state, -2, "is_dir");
        lua_pushinteger(state, info.st_size);
        lua_setfield(state, -2, "size");
        lua_pushinteger(state, info.st_mtim.tv_sec);
        lua_setfield(state, -2, "mtime");
        lua_pushinteger(state, FileStamp(info));
        lua_setfield(state, -2, "stamp");
        lua_pushboolean(state, S_ISLNK(info.st_mode));
        lua_setfield(state, -2, "reparse");
        return 1;
    }

    static int FsMkdir(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsBoolError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path || path->filename().empty()) return FsBoolError(state, "路径无效");
        const DirectoryHandle parent(path->parent_path());
        if (parent.get() < 0 || ::mkdirat(parent.get(), path->filename().c_str(), 0700) != 0) {
            return FsBoolError(state, "无法创建目录");
        }
        lua_pushboolean(state, true);
        return 1;
    }

    static int FsList(lua_State* state) {
        if (!Self(state).runtime_.Connected()) return FsError(state, "未连接");
        const auto path_arg = CheckString(state, 1);
        const bool strict = lua_toboolean(state, 2);
        const auto limit = strict ? static_cast<std::size_t>(std::clamp<lua_Integer>(
            luaL_optinteger(state, 3, max_fs_list_entries), 0, max_fs_list_entries)) : max_fs_list_entries;
        const auto path = SafeFsPath(path_arg);
        if (!path) return FsError(state, "路径无效");
        const DirectoryHandle directory(*path);
        if (directory.get() < 0) return FsError(state, "无法访问目录");
        const auto entry_fd = ::fcntl(directory.get(), F_DUPFD_CLOEXEC, 0);
        if (entry_fd < 0) return FsError(state, "无法读取目录");
        const auto close_directory = [](DIR* value) { ::closedir(value); };
        std::unique_ptr<DIR, decltype(close_directory)> entries(::fdopendir(entry_fd), close_directory);
        if (!entries) {
            ::close(entry_fd);
            return FsError(state, "无法读取目录");
        }
        struct Entry {
            std::string name;
            bool is_directory;
            off_t size;
            time_t mtime;
        };
        std::vector<Entry> values;
        bool truncated{};
        while (const auto* entry = ::readdir(entries.get())) {
            const std::string_view name(entry->d_name);
            if (name == "." || name == ".." || name.starts_with(".p2ptmp.") || !Utf8ToWide(name)) continue;
            struct stat info{};
            if (::fstatat(directory.get(), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0 ||
                (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) continue;
            if (values.size() == limit) {
                truncated = true;
                break;
            }
            values.push_back({std::string(name), S_ISDIR(info.st_mode), info.st_size, info.st_mtim.tv_sec});
        }
        entries.reset();
        std::ranges::sort(values, [](const auto& a, const auto& b) {
            if (a.is_directory != b.is_directory) return a.is_directory;
            return a.name < b.name;
        });
        lua_createtable(state, 0, 4);
        PushString(state, path->native());
        lua_setfield(state, -2, "path");
        PushString(state, path->parent_path() == *path ? std::string{} : path->parent_path().native());
        lua_setfield(state, -2, "parent");
        lua_pushboolean(state, truncated);
        lua_setfield(state, -2, "truncated");
        lua_createtable(state, static_cast<int>(values.size()), 0);
        lua_Integer index = 1;
        for (const auto& item : values) {
            lua_createtable(state, 0, 5);
            PushString(state, item.name);
            lua_setfield(state, -2, "name");
            lua_pushboolean(state, item.is_directory);
            lua_setfield(state, -2, "is_dir");
            lua_pushinteger(state, item.size);
            lua_setfield(state, -2, "size");
            lua_pushinteger(state, item.mtime);
            lua_setfield(state, -2, "mtime");
            PushString(state, QDateTime::fromSecsSinceEpoch(item.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm")).toStdString());
            lua_setfield(state, -2, "mtime_text");
            lua_rawseti(state, -2, index++);
        }
        lua_setfield(state, -2, "entries");
        return 1;
    }

    static FileSlot* FileById(LuaPlugin& self, lua_Integer id) {
        const auto slot = std::ranges::find(self.files_, id, &FileSlot::id);
        return slot != self.files_.end() ? &*slot : nullptr;
    }

    static int FsOpenRead(lua_State* state) {
        auto& self = Self(state);
        if (!self.runtime_.Connected()) return FsError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path) return FsError(state, "路径无效");
        auto* slot = FileById(self, 0);
        struct stat info{};
        if (!slot || !slot->openRead(*path, info)) return FsError(state, "无法打开此文件");
        slot->id = ++self.next_file_;
        lua_pushinteger(state, slot->id);
        lua_pushinteger(state, info.st_size);
        if (lua_toboolean(state, 2)) {
            lua_pushinteger(state, FileStamp(info));
            return 3;
        }
        return 2;
    }

    static int FsOpenWrite(lua_State* state) {
        auto& self = Self(state);
        if (!self.runtime_.Connected()) return FsError(state, "未连接");
        const auto path = SafeFsPath(CheckString(state, 1));
        if (!path) return FsError(state, "路径无效");
        auto* slot = FileById(self, 0);
        if (!slot || !slot->openWrite(*path)) return FsError(state, "无法写入此文件");
        slot->id = ++self.next_file_;
        lua_pushinteger(state, slot->id);
        return 1;
    }

    static int FsSeek(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        const auto offset = luaL_checkinteger(state, 2);
        auto* slot = id ? FileById(self, id) : nullptr;
        if (!self.runtime_.Connected() || !slot || slot->writable || !slot->seek(offset)) {
            return FsBoolError(state, "无效的读取句柄或偏移量");
        }
        lua_pushboolean(state, true);
        return 1;
    }

    static int FsRead(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        const auto requested = luaL_optinteger(state, 2, 32 * 1024);
        auto* slot = id ? FileById(self, id) : nullptr;
        if (!self.runtime_.Connected() || !slot || slot->writable || requested < 1) {
            return FsError(state, "无效的读取句柄");
        }
        const auto bytes = slot->read(std::min<lua_Integer>(requested, max_fs_read_chunk));
        if (!bytes) return FsError(state, "文件读取失败");
        lua_pushlstring(state, bytes->constData(), bytes->size());
        return 1;
    }

    static int FsWrite(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        const auto data = CheckString(state, 2);
        auto* slot = id ? FileById(self, id) : nullptr;
        if (!self.runtime_.Connected() || !slot || !slot->writable || !slot->write(data)) {
            return FsBoolError(state, "文件写入失败");
        }
        lua_pushboolean(state, true);
        return 1;
    }

    static int FsCommit(lua_State* state) {
        auto& self = Self(state);
        const auto id = luaL_checkinteger(state, 1);
        auto* slot = id ? FileById(self, id) : nullptr;
        if (!self.runtime_.Connected() || !slot || !slot->writable || !slot->commit()) {
            return FsBoolError(state, "无法保存文件");
        }
        lua_pushboolean(state, true);
        return 1;
    }

    static int FsHash(lua_State* state) {
        const auto id = luaL_checkinteger(state, 1);
        auto* slot = id ? FileById(Self(state), id) : nullptr;
        const auto result = slot ? slot->hashResult() : QByteArray{};
        if (result.isEmpty()) lua_pushnil(state);
        else lua_pushlstring(state, result.constData(), result.size());
        return 1;
    }

    static int FsClose(lua_State* state) {
        const auto id = luaL_checkinteger(state, 1);
        auto* slot = id ? FileById(Self(state), id) : nullptr;
        if (slot) slot->clear();
        lua_pushboolean(state, slot != nullptr);
        return 1;
    }
