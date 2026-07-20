# WeChat-Hook
 
Windows 微信 Hook DLL，当前版本以 `version.dll` 代理方式加载，并在微信主进程中提供 ImGui 控制面板及本地 HTTP 服务。

## 当前能力

- 新增基于 Dear ImGui、Win32 和 DirectX 11 的独立控制面板，可用于查看运行状态和调用项目功能。
- 更新微信 Hook 消息发送能力。目前仅支持发送文本消息。
- 发送文本消息时必须提供明确的接收目标：
  - 好友或其他单聊目标：使用对方的 `wxid`。
  - 群聊目标：使用对应的 `chatroom` 号，通常以 `@chatroom` 结尾。
- 支持通过 ImGui 控制面板或本地 HTTP 接口调用文本发送能力。

> 当前版本的 Hook 发送能力仅限文本消息。图片、文件、语音、小程序等消息类型暂未在本次更新范围内。

## 后续计划

项目仍在持续开发中，后续将根据研究进度逐步更新消息能力、防撤回等功能。微信版本升级可能导致内部接口和偏移发生变化，相关功能需要重新适配。

## 免责声明

本项目仅供技术研究与学习交流使用，不得用于任何商业用途或违法用途。使用者应遵守所在地法律法规以及相关软件的用户协议，并自行承担因使用本项目产生的一切责任。项目作者不对任何滥用行为及其造成的后果负责。

## 版本说明

- 目标微信版本：`4.1.11.24`
- 架构：Windows x64
- DLL 名称：`version.dll`
- 默认监听地址：`0.0.0.0:30001`
- 当前 main 分支已移除 VMP 保护、远程授权校验、PB/NetSceneSendPB、CDN、WcProbe/CCD/NtQuery 相关 Hook 安装与处理代码。

## 使用方式

1. 编译或从 Release 下载生成后的 `version.dll`。
2. 将 `version.dll` 放到微信安装目录下，例如：

```text
C:\Program Files\Tencent\Weixin
```

3. 启动微信。DLL 加载后会自动启动 HTTP 服务，默认端口是 `30001`。
4. 使用 Postman 或其他 HTTP 客户端调用接口，默认基地址：

```text
http://127.0.0.1:30001
```

## 项目结构

- `dllmain.cpp`：DLL 入口，解析启动参数，加载真实系统 `version.dll`，只在微信主进程初始化。
- `src/version_proxy.cpp`：代理系统 `version.dll` 导出函数。
- `src/inline_weixin_dll_load.cpp`：等待并初始化 `Weixin.dll` 相关逻辑，启动 HTTP 服务。
- `src/http_routes.cpp`：HTTP 路由注册入口。
- `src/SendTextMsg.cpp`：文本消息 HTTP 接口。
- `src/wx_send_qt.cpp`：微信 Qt 文本发送逻辑。
- `src/imgui_ui.cpp`：ImGui 控制面板。

## 编译

使用 Visual Studio / MSBuild 编译：

```powershell
MSBuild.exe x64_Version_dll.vcxproj /m /t:Build /p:Configuration=Release /p:Platform=x64
```

Release 输出：

```text
x64\Release\version.dll
```

Debug 输出：

```text
x64\Debug\version.dll
```

## ImGui 控制面板

项目内置 Dear ImGui v1.92.5，并使用 Win32 + DirectX 11 后端创建独立控制面板。DLL 在微信主进程完成初始化后自动显示窗口；按 `Insert` 可随时显示或隐藏。

面板采用上位机控制台布局，提供运行状态和功能调用页面。当前已更新并验证的 Hook 发送能力仅支持文本消息；发送时需要填写好友 `wxid` 或群聊 `chatroom` 号。调用在后台执行，响应统一显示在结果区域。关闭窗口只会隐藏面板，不会停止 Hook 或 HTTP 服务。

文本发送页使用微信自身 Qt queued callback 执行内部 orchestrator。controller resolver 使用三次 Pointer Map 重扫后仍存活的 5 条最短静态 `Weixin.dll` 候选链，并按 CE MemoryRecord 的 final-first offset 顺序逐条求值。调试区实时显示命中的候选编号、逐级地址以及 `active/status/callback/inject/recipient/destroy/oldRelease/task`；`status=3` 表示已进入原始提交调用。

## 加载与启动参数

项目生成的 DLL 文件名为 `version.dll`。加载后会代理系统 `version.dll` 的导出函数，并在微信主进程内初始化 Hook 与 HTTP 服务。

支持的命令行参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `StartPort=` | `30001` | HTTP 服务监听端口 |
| `RecvType=` | `1` | 接收回调类型参数，当前保留解析 |
| `CallBackURL=` | 空 | 接收回调地址，当前用于保存配置和端口解析 |

示例：

```text
WeChat.exe StartPort=30001 CallBackURL="http://127.0.0.1:8080/callback"
```

## HTTP 接口

当前公开说明的接口仅包含文本消息发送，默认基地址：

```text
http://127.0.0.1:30001
```

### 发送文本消息

`POST /SendTextMsg`

请求：

```json
{
  "wxidorgid": "wxid_xxx",
  "msg": "hello"
}
```

响应：

```json
{
  "ret": 0,
  "retmsg": "queued"
}
```

`wxidorgid` 可填写好友 `wxid`，也可填写以 `@chatroom` 结尾的群聊号。

curl：

```bash
curl -X POST http://127.0.0.1:30001/SendTextMsg \
  -H "Content-Type: application/json" \
  -d "{\"wxidorgid\":\"wxid_xxx\",\"msg\":\"hello\"}"
```

## 返回约定

- JSON 解析失败时，接口返回：

```json
{
  "ret": -1,
  "msg": "invalid json"
}
```

- 文本发送接口使用 `ret` / `retmsg` 作为返回字段。
- `ret: 0`、`retmsg: "queued"` 表示文本发送任务已成功加入执行队列。
- `ret: -1`、`retmsg: "queue failed"` 表示任务加入执行队列失败。

## 备注

本项目依赖微信内部偏移，微信升级后需要重新核对偏移和调用约定。当前 README 与 Release DLL 对应微信 `4.1.11.24`。
