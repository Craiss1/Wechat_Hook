#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "global.h"
#include "httplib.h"
#include "imgui_ui.h"
#include "json.hpp"
#include "wx_send_qt.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr wchar_t kWindowClass[] = L"WeChatHookImGuiWindow";
    constexpr wchar_t kWindowTitle[] = L"WeChat Hook Control Panel";

    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_deviceContext = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;
    ID3D11RenderTargetView* g_renderTarget = nullptr;
    UINT g_resizeWidth = 0;
    UINT g_resizeHeight = 0;
    bool g_swapChainOccluded = false;
    LONG g_threadStarted = 0;

    enum class ControlPage
    {
        Status,
        SendText,
        SendImage,
        ForwardXml,
        DecodeImage,
        Profile,
        Database
    };

    struct ControlState
    {
        ControlPage page = ControlPage::Status;
        char recipient[256]{};
        char message[4096]{};
        char imagePath[1024]{};
        char xmlRecipient[256]{};
        char xmlContent[16384]{};
        char decodeSource[1024]{};
        char decodeTarget[1024]{};
        char databaseName[256] = "MicroMsg.db";
        char sql[8192] = "SELECT * FROM ChatRoom LIMIT 10";
    };

    ControlState g_controlState;
    std::atomic_bool g_requestBusy = false;
    std::mutex g_resultMutex;
    std::string g_requestResult = "等待操作。";

    void SetResult(std::string result)
    {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        g_requestResult = std::move(result);
    }

    std::string GetResult()
    {
        std::lock_guard<std::mutex> lock(g_resultMutex);
        return g_requestResult;
    }

    void PostApiAsync(std::string path, std::string body)
    {
        if (g_requestBusy.exchange(true))
            return;

        SetResult("正在调用，请稍候……");
        std::thread([path = std::move(path), body = std::move(body)]()
        {
            try
            {
                httplib::Client client("127.0.0.1", g_StartPort);
                client.set_connection_timeout(3, 0);
                client.set_read_timeout(30, 0);
                client.set_write_timeout(10, 0);

                auto response = client.Post(path, body, "application/json");
                if (response)
                {
                    std::string result = "HTTP " + std::to_string(response->status) + "\n\n";
                    result += response->body.empty() ? "(空响应)" : response->body;
                    SetResult(std::move(result));
                }
                else
                {
                    SetResult("调用失败：" + httplib::to_string(response.error()));
                }
            }
            catch (const std::exception& exception)
            {
                SetResult(std::string("调用异常：") + exception.what());
            }
            catch (...)
            {
                SetResult("调用异常：未知错误");
            }

            g_requestBusy = false;
        }).detach();
    }

    void PostJsonAsync(const char* path, const nlohmann::json& body)
    {
        PostApiAsync(path, body.dump());
    }

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        {
            g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
            backBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (g_renderTarget)
        {
            g_renderTarget->Release();
            g_renderTarget = nullptr;
        }
    }

    bool CreateDevice(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC swapDesc{};
        swapDesc.BufferCount = 2;
        swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.OutputWindow = hwnd;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.Windowed = TRUE;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selectedLevel{};

        HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            levels,
            static_cast<UINT>(_countof(levels)),
            D3D11_SDK_VERSION,
            &swapDesc,
            &g_swapChain,
            &g_device,
            &selectedLevel,
            &g_deviceContext);

        if (result == DXGI_ERROR_UNSUPPORTED)
        {
            result = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                0,
                levels,
                static_cast<UINT>(_countof(levels)),
                D3D11_SDK_VERSION,
                &swapDesc,
                &g_swapChain,
                &g_device,
                &selectedLevel,
                &g_deviceContext);
        }

        if (FAILED(result))
            return false;

        CreateRenderTarget();
        return g_renderTarget != nullptr;
    }

    void CleanupDevice()
    {
        CleanupRenderTarget();
        if (g_swapChain)
        {
            g_swapChain->Release();
            g_swapChain = nullptr;
        }
        if (g_deviceContext)
        {
            g_deviceContext->Release();
            g_deviceContext = nullptr;
        }
        if (g_device)
        {
            g_device->Release();
            g_device = nullptr;
        }
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
            return TRUE;

        switch (message)
        {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
            {
                g_resizeWidth = LOWORD(lParam);
                g_resizeHeight = HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_GETMINMAXINFO:
        {
            auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
            minMaxInfo->ptMinTrackSize.x = 520;
            minMaxInfo->ptMinTrackSize.y = 360;
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void ApplyStyle()
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 5.0f;
        style.GrabRounding = 5.0f;
        style.WindowPadding = ImVec2(16.0f, 16.0f);
        style.ItemSpacing = ImVec2(10.0f, 8.0f);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.078f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.07f, 0.55f, 0.32f, 0.75f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.08f, 0.68f, 0.39f, 0.85f);
        colors[ImGuiCol_Button] = ImVec4(0.07f, 0.55f, 0.32f, 0.85f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.08f, 0.68f, 0.39f, 1.0f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.06f, 0.45f, 0.27f, 1.0f);
    }

    bool NavigationItem(const char* label, ControlPage page)
    {
        const bool selected = g_controlState.page == page;
        if (ImGui::Selectable(label, selected, 0, ImVec2(0.0f, 42.0f)))
            g_controlState.page = page;
        return selected;
    }

    void DrawStatusPage(HWND hwnd)
    {
        const bool loaded = g_hWeixinDll != nullptr;
        const bool loggedIn = g_IsLogin == 1;
        const ImVec4 ok(0.20f, 0.85f, 0.47f, 1.0f);
        const ImVec4 waiting(0.95f, 0.70f, 0.20f, 1.0f);

        ImGui::TextUnformatted("运行状态");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted("Weixin.dll");
        ImGui::SameLine(170.0f);
        ImGui::TextColored(loaded ? ok : waiting, loaded ? "已加载" : "等待加载");
        ImGui::TextUnformatted("账号状态");
        ImGui::SameLine(170.0f);
        ImGui::TextColored(loggedIn ? ok : waiting, loggedIn ? "已登录" : "未登录");
        ImGui::Text("进程 ID             %lu", static_cast<unsigned long>(GetCurrentProcessId()));
        ImGui::Text("HTTP 地址           http://127.0.0.1:%d", g_StartPort);

        ImGui::Spacing();
        if (ImGui::Button("复制 HTTP 地址"))
        {
            char endpoint[64]{};
            sprintf_s(endpoint, "http://127.0.0.1:%d", g_StartPort);
            ImGui::SetClipboardText(endpoint);
        }
        ImGui::SameLine();
        if (ImGui::Button("隐藏窗口（Insert）"))
            ShowWindow(hwnd, SW_HIDE);
    }

    void DrawSendTextPage()
    {
        const WeixinSend::QtTextSendState state = WeixinSend::GetQtTextSendState();

        ImGui::TextUnformatted("发送文本消息");
        ImGui::Separator();
        ImGui::Text("Hook: %s", state.installed ? "ready" : "not installed");
        ImGui::Text("controller: %016llX", static_cast<unsigned long long>(state.controller));
        ImGui::Text(
            "candidate=%d root=%016llX failLevel=%d",
            state.controllerCandidate,
            static_cast<unsigned long long>(state.controllerRoot),
            state.controllerFailureLevel);
        ImGui::Text(
            "L1=%016llX L2=%016llX L3=%016llX",
            static_cast<unsigned long long>(state.controllerLevels[0]),
            static_cast<unsigned long long>(state.controllerLevels[1]),
            static_cast<unsigned long long>(state.controllerLevels[2]));
        ImGui::Text(
            "L4=%016llX L5=%016llX",
            static_cast<unsigned long long>(state.controllerLevels[3]),
            static_cast<unsigned long long>(state.controllerLevels[4]));
        ImGui::Text(
            "active=%ld status=%ld callback=%ld inject=%ld recipient=%ld destroy=%ld oldRelease=%ld",
            state.active,
            state.status,
            state.callback,
            state.inject,
            state.recipient,
            state.destroy,
            state.oldRelease);
        ImGui::Text("task: %016llX", static_cast<unsigned long long>(state.lastTask));
        ImGui::Spacing();
        ImGui::InputText("接收人 wxid / 群 ID", g_controlState.recipient, sizeof(g_controlState.recipient));
        ImGui::InputTextMultiline("消息内容", g_controlState.message, sizeof(g_controlState.message), ImVec2(-1.0f, 180.0f));

        if (ImGui::Button("发送文本", ImVec2(140.0f, 38.0f)))
        {
            if (g_controlState.recipient[0] == '\0' || g_controlState.message[0] == '\0')
                SetResult("请填写接收人和消息内容。");
            else
                SetResult(WeixinSend::QueueQtText(
                    g_controlState.recipient,
                    g_controlState.message) ? "消息已进入微信 Qt 队列。" : "入队失败，请查看上方状态值。");
        }
    }

    void DrawSendImagePage()
    {
        ImGui::TextUnformatted("发送图片消息");
        ImGui::Separator();
        ImGui::InputText("接收人 wxid / 群 ID", g_controlState.recipient, sizeof(g_controlState.recipient));
        ImGui::InputText("图片完整路径", g_controlState.imagePath, sizeof(g_controlState.imagePath));

        if (ImGui::Button("发送图片", ImVec2(140.0f, 38.0f)))
        {
            if (g_controlState.recipient[0] == '\0' || g_controlState.imagePath[0] == '\0')
                SetResult("请填写接收人和图片路径。");
            else
                PostJsonAsync("/SendImgMsg", {
                    {"wxidorgid", g_controlState.recipient},
                    {"path", g_controlState.imagePath}
                });
        }
    }

    void DrawForwardXmlPage()
    {
        ImGui::TextUnformatted("转发 XML 消息");
        ImGui::Separator();
        ImGui::InputText("接收人 wxid", g_controlState.xmlRecipient, sizeof(g_controlState.xmlRecipient));
        ImGui::InputTextMultiline("XML 内容", g_controlState.xmlContent, sizeof(g_controlState.xmlContent), ImVec2(-1.0f, 210.0f));

        if (ImGui::Button("转发 XML", ImVec2(140.0f, 38.0f)))
        {
            if (g_controlState.xmlRecipient[0] == '\0' || g_controlState.xmlContent[0] == '\0')
                SetResult("请填写接收人和 XML 内容。");
            else
                PostJsonAsync("/ForwardXMLMsg", {
                    {"to_wxid", g_controlState.xmlRecipient},
                    {"content", g_controlState.xmlContent}
                });
        }
    }

    void DrawDecodeImagePage()
    {
        ImGui::TextUnformatted("解码微信图片");
        ImGui::Separator();
        ImGui::InputText("源 DAT 文件", g_controlState.decodeSource, sizeof(g_controlState.decodeSource));
        ImGui::InputText("输出图片路径", g_controlState.decodeTarget, sizeof(g_controlState.decodeTarget));

        if (ImGui::Button("开始解码", ImVec2(140.0f, 38.0f)))
        {
            if (g_controlState.decodeSource[0] == '\0' || g_controlState.decodeTarget[0] == '\0')
                SetResult("请填写源文件和输出路径。");
            else
                PostJsonAsync("/Decode_Pic", {
                    {"src_path", g_controlState.decodeSource},
                    {"dst_path", g_controlState.decodeTarget}
                });
        }
    }

    void DrawProfilePage()
    {
        ImGui::TextUnformatted("当前账号资料");
        ImGui::Separator();
        ImGui::TextWrapped("点击按钮读取当前登录账号的 wxid、昵称、手机号、地区等资料。");
        ImGui::Spacing();
        if (ImGui::Button("读取账号资料", ImVec2(160.0f, 38.0f)))
            PostJsonAsync("/GetSelfProfile", nlohmann::json::object());
    }

    void DrawDatabasePage()
    {
        ImGui::TextUnformatted("数据库查询");
        ImGui::Separator();
        ImGui::InputText("数据库名称", g_controlState.databaseName, sizeof(g_controlState.databaseName));
        ImGui::InputTextMultiline("SQL", g_controlState.sql, sizeof(g_controlState.sql), ImVec2(-1.0f, 170.0f));

        if (ImGui::Button("执行 SQL", ImVec2(140.0f, 38.0f)))
        {
            if (g_controlState.databaseName[0] == '\0' || g_controlState.sql[0] == '\0')
                SetResult("请填写数据库名称和 SQL。");
            else
                PostJsonAsync("/QueryDB/execute", {
                    {"optDbName", g_controlState.databaseName},
                    {"SQL", g_controlState.sql}
                });
        }
        ImGui::SameLine();
        if (ImGui::Button("获取数据库列表", ImVec2(180.0f, 38.0f)))
            PostJsonAsync("/QueryDB/GetAllDBName", nlohmann::json::object());
    }

    void DrawCurrentPage(HWND hwnd)
    {
        switch (g_controlState.page)
        {
        case ControlPage::Status:      DrawStatusPage(hwnd); break;
        case ControlPage::SendText:    DrawSendTextPage(); break;
        case ControlPage::SendImage:   DrawSendImagePage(); break;
        case ControlPage::ForwardXml:  DrawForwardXmlPage(); break;
        case ControlPage::DecodeImage: DrawDecodeImagePage(); break;
        case ControlPage::Profile:     DrawProfilePage(); break;
        case ControlPage::Database:    DrawDatabasePage(); break;
        }
    }

    void DrawControlPanel(HWND hwnd)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::Begin("WeChat Hook", nullptr, flags);

        ImGui::BeginChild("Navigation", ImVec2(190.0f, 0.0f), true);
        ImGui::TextUnformatted("WECHAT HOOK");
        ImGui::TextDisabled("上位机控制台");
        ImGui::Separator();
        NavigationItem("运行状态", ControlPage::Status);
        NavigationItem("发送文本", ControlPage::SendText);
        NavigationItem("发送图片", ControlPage::SendImage);
        NavigationItem("转发 XML", ControlPage::ForwardXml);
        NavigationItem("图片解码", ControlPage::DecodeImage);
        NavigationItem("账号资料", ControlPage::Profile);
        NavigationItem("数据库", ControlPage::Database);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("MainWorkspace", ImVec2(0.0f, 0.0f), false);

        const float resultHeight = 190.0f;
        ImGui::BeginChild("FunctionPanel", ImVec2(0.0f, -resultHeight), true);
        ImGui::BeginDisabled(g_requestBusy.load());
        DrawCurrentPage(hwnd);
        ImGui::EndDisabled();
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::TextUnformatted(g_requestBusy.load() ? "调用结果（执行中）" : "调用结果");
        ImGui::SameLine();
        if (ImGui::SmallButton("清空"))
            SetResult("");
        ImGui::BeginChild("ResultPanel", ImVec2(0.0f, 0.0f), true);
        const std::string result = GetResult();
        ImGui::TextWrapped("%s", result.c_str());
        ImGui::EndChild();

        ImGui::EndChild();
        ImGui::End();
    }

    DWORD WINAPI UiThreadProc(void*)
    {
        WNDCLASSEXW windowClass{
            sizeof(WNDCLASSEXW),
            CS_CLASSDC,
            WindowProc,
            0,
            0,
            g_hModule,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            kWindowClass,
            nullptr
        };

        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            InterlockedExchange(&g_threadStarted, 0);
            return 1;
        }

        constexpr int windowWidth = 960;
        constexpr int windowHeight = 640;
        const int windowX = (GetSystemMetrics(SM_CXSCREEN) - windowWidth) / 2;
        const int windowY = (GetSystemMetrics(SM_CYSCREEN) - windowHeight) / 2;

        HWND hwnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            kWindowClass,
            kWindowTitle,
            WS_OVERLAPPEDWINDOW,
            windowX,
            windowY,
            windowWidth,
            windowHeight,
            nullptr,
            nullptr,
            g_hModule,
            nullptr);

        if (!hwnd || !CreateDevice(hwnd))
        {
            CleanupDevice();
            if (hwnd)
                DestroyWindow(hwnd);
            UnregisterClassW(kWindowClass, g_hModule);
            InterlockedExchange(&g_threadStarted, 0);
            return 1;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        {
            io.Fonts->AddFontFromFileTTF(
                "C:\\Windows\\Fonts\\msyh.ttc",
                18.0f,
                nullptr,
                io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
        ApplyStyle();

        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX11_Init(g_device, g_deviceContext);

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        bool done = false;
        bool visible = true;
        while (!done)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                if (message.message == WM_QUIT)
                    done = true;
            }
            if (done)
                break;

            if ((GetAsyncKeyState(VK_INSERT) & 1) != 0)
            {
                visible = !IsWindowVisible(hwnd);
                ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
                if (visible)
                {
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd);
                }
            }

            visible = IsWindowVisible(hwnd) != FALSE;
            if (!visible)
            {
                Sleep(30);
                continue;
            }

            if (g_swapChainOccluded &&
                g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
            {
                Sleep(10);
                continue;
            }
            g_swapChainOccluded = false;

            if (g_resizeWidth != 0 && g_resizeHeight != 0)
            {
                CleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeWidth = 0;
                g_resizeHeight = 0;
                CreateRenderTarget();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            DrawControlPanel(hwnd);
            ImGui::Render();

            constexpr float clearColor[4] = { 0.055f, 0.063f, 0.078f, 1.0f };
            g_deviceContext->OMSetRenderTargets(1, &g_renderTarget, nullptr);
            g_deviceContext->ClearRenderTargetView(g_renderTarget, clearColor);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            const HRESULT presentResult = g_swapChain->Present(1, 0);
            g_swapChainOccluded = presentResult == DXGI_STATUS_OCCLUDED;
            if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET)
                done = true;
        }

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDevice();
        DestroyWindow(hwnd);
        UnregisterClassW(kWindowClass, g_hModule);
        InterlockedExchange(&g_threadStarted, 0);
        return 0;
    }
}

bool StartImGuiUi()
{
    if (InterlockedCompareExchange(&g_threadStarted, 1, 0) != 0)
        return true;

    HANDLE thread = CreateThread(nullptr, 0, UiThreadProc, nullptr, 0, nullptr);
    if (!thread)
    {
        InterlockedExchange(&g_threadStarted, 0);
        return false;
    }

    CloseHandle(thread);
    return true;
}
