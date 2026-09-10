# 标准目录变量保留发行版与用户通过命令行覆盖安装布局的能力；规则不得依赖 调用者当前工作目录，也不执行下载或修改源树的外部命令。
include(GNUInstallDirs)

# Windows/Linux 的 RUNTIME 进入标准可执行目录，macOS bundle 则落在包根， 以便 DragNDrop 直接包含完整
# .app。当前客户端作为单一组件交付。 这里只安装最终客户端，不导出内部库、开发头、CMake package 或 pkg-config。
install(
  TARGETS AudioRoads
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  BUNDLE DESTINATION ".")

# freedesktop 桌面入口只属于 Linux/Unix 桌面包，不能进入 Windows ZIP 或 macOS 镜像。未来图标和 AppStream
# 元数据也应紧邻该平台规则安装。 Exec、Name 与类别必须和最终可执行产物保持一致。
if(UNIX AND NOT APPLE)
  install(FILES "packaging/io.github.xiangcao233.audioroads.desktop"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
endif()

# 包名、供应商与 project 元数据集中设置，后续若在文件名中增加平台或架构， 也应继续通过 CPACK_* 变量生成，不能另写脚本拼接。
# 产品版本直接复用 project(VERSION)，不维护第二份版本常量。
set(CPACK_PACKAGE_NAME "AudioRoads")
set(CPACK_PACKAGE_VENDOR "AudioRoads")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

# 显式选择生成器，避免 CPack 按构建主机猜测交付格式。
if(APPLE)
  # DragNDrop 只定义镜像容器；签名、公证、framework 引用和 bundle 启动仍需 在真实 macOS 交付环境单独验收。
  set(CPACK_GENERATOR "DragNDrop")
elseif(WIN32)
  # 尚未定义安装器升级/卸载策略，先用透明 ZIP，且交付时必须检查 DLL 完整性 与 GUI 子系统无控制台启动行为。
  set(CPACK_GENERATOR "ZIP")
else()
  # Linux 先使用中立 TGZ；引入 DEB/RPM 后应继续拆分平台脚本，并检查 ELF NEEDED、RPATH/RUNPATH
  # 和目标发行版兼容性。
  set(CPACK_GENERATOR "TGZ")
endif()

# CPack 在 include 时读取全部 CPACK_* 变量并生成 package target，因此必须位于
# 所有元数据和平台分支之后。普通编译成功不能替代 cpack 与包内容检查。 安装验证应使用独立 DESTDIR，并检查权限、目录层级及无构建树启动；包内不得
# 意外包含测试程序、编译中间文件、上游源码或仓库根 include 目录。
include(CPack)
