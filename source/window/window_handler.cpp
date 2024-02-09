#include <windows.h>
#include <window/window_handler.h>

struct window_state
{
    HWND    handle;

    u32     width;
    u32     height;
};

struct ghost_thread
{
    BOOL    ghost_thread_runtime;
    BOOL    ghost_thread_ready;
    DWORD   ghost_thread_identifier;
    HANDLE  ghost_thread_sync;
    HWND    ghost_window;

    DWORD   windows_active;
};

struct window_creation_context
{
    DWORD       ex_style;
    LPCWSTR     class_name;
    LPCWSTR     window_name;
    DWORD       style;
    int         x;
    int         y;
    int         width;
    int         height;
    HWND        parent_window;
    HMENU       menu;
    HINSTANCE   instance;
    LPVOID      user_parameter;
};

#define J5_WINDOW_CAST(handle) (window_state *)handle

#define UD_CREATE_WINDOW    (WM_USER + 0x0042)
#define UD_DESTROY_WINDOW   (WM_USER + 0x0043)
#define UD_UPDATE_WINDOW    (WM_USER + 0x0044)

static DWORD            main_thread_id;
static ghost_thread*    ghost_thread_state;

// --- Internal Helpers --------------------------------------------------------

static LRESULT CALLBACK
wDisplayWindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{

    LRESULT ret_result = 0;   

    // Essentially, our window procedure catches the messages and then forwards
    // them off as a thread message to our runtime thread, effectively making it
    // non-blocking since the message can be sent and allow the window procedure
    // to keep going.
    switch (message)
    {
        
        // For close, we simply just smuggle the window handle with w_param.
        case WM_CLOSE:
        {
            PostThreadMessageW(main_thread_id, message, (WPARAM)window, l_param);
            break;
        };

        // For all other messages that we want to process, we just send it over.
        // TODO(Chris): We need to pack these messages with the window handle so
        // that we know what messages correspond to what window.
        case WM_DESTROY:
        {
            PostThreadMessageW(main_thread_id, message, w_param, l_param);
            break;
        };

        // Anything we don't care about is defaulted.
        default:
        {
            ret_result = DefWindowProcW(window, message, w_param, l_param);
        };

    };

    return ret_result;

}

static LRESULT CALLBACK
wGhostWindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{

    LRESULT ret_result = 0;

    switch (message)
    {
        case UD_CREATE_WINDOW:
        {

            // Cast to the creation context and then run it.
            window_creation_context *context = (window_creation_context*)w_param;
            HWND window_handle = CreateWindowExW(context->ex_style, context->class_name,
                    context->window_name, context->style, context->x, context->y, context->width,
                    context->height, context->parent_window, context->menu, context->instance,
                    context->user_parameter);

            // The result can be packed as a LRESULT, which is kinda neat.
            if (window_handle != NULL)
                ghost_thread_state->active_windows++;
            ret_result = (LRESULT)window_handle;
            break;

        };

        case UD_DESTROY_WINDOW:
        {

            // A request to destroy the window is made.
            if (DestroyWindow((HWND)w_param))
                ghost_thread_state->active_windows--;

            break;

        };

        default:
        {

            // Generally, any other event that the ghost window receives can
            // be defaulted. We don't really care about this window's existence.
            ret_result = DefWindowProcW(window, message, w_param, l_param);
            break;

        };
    };

    return ret_result;

}

static DWORD WINAPI
window_ghost_main(LPVOID user_param)
{

    // We need to create our ghost window class.
    WNDCLASSEXW ghost_window_class    = {};
    ghost_window_class.cbSize         = sizeof(ghost_window_class);
    ghost_window_class.lpfnWndProc    = &wGhostWindowProc;
    ghost_window_class.hInstance      = GetModuleHandleW(NULL);
    ghost_window_class.hIcon          = LoadIconA(NULL, IDI_APPLICATION);
    ghost_window_class.hCursor        = LoadCursorA(NULL, IDC_ARROW);
    ghost_window_class.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    ghost_window_class.lpszClassName  = L"ghostWindowClass";
    RegisterClassExW(&ghost_window_class);

    // Our creation routine is fairly normal, except for the fact we don't specify
    // any styling; effectively making this window hidden. This is intentional, as
    // we only want this ghost window to capture messages for us.
    HWND ghost_window_handle = CreateWindowExW(0, ghost_window_class.lpszClassName,
            L"Ghost Window", 0, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            0, 0, ghost_window_class.hInstance, 0);
    ghost_window_state->ghost_window = ghost_window_handle;
    
    // Signals back to the main thread after sync that it is valid, otherwise, it asserts.
    if (ghost_window_handle)
        ghost_window->ghost_thread_ready = true;
    else
        ExitProcess(1);

    // This signals back to the main thread that the ghost window thread's is done
    // doing its essential set up.
    SetEvent(ghost_window->ghost_thread_sync);

    // This is the message pump that the windows all forward out.
    while(ghost_window->ghost_thread_runtime)
    {

        MSG current_message = {};
        GetMessageW(&current_message, 0, 0, 0);
        TranslateMessage(&current_message);

        // These messages are forwarded back to main to be captured.
        if ((current_message.message == WM_CHAR) ||
            (current_message.message == WM_QUIT) ||
            (current_message.message == WM_SIZE))
        {
            PostThreadMessageW(main_thread_id, current_message.message,
                    current_message.wParam, current_message.lParam);
        }
        else
        {
            DispatchMessageW(&current_message);
        }

    }


    // The ghost thread will probably never exit naturally, but this is here in
    // case it does.
    ExitProcess(0);
    

}

// --- Window Handler API ------------------------------------------------------
//
// Window initialize startup must be called before windows can be generated. An
// easy check can be done when the first call to create window is done or explicitly.
// A static reference to the state is made such that it is visible once complete.
//

bool
window_initialize_startup(void)
{

    // In the event this is called more than once, just return the ready status.
    if (ghost_thread_state != NULL)
        return ghost_thread_state->ghost_thread_ready;

    // We need to get the main thread identifier so our ghost thread
    // knows where to post messages.
    main_thread_id = GetCurrentThreadId();

    // Create the ghost thread state structure.
    ghost_thread_state = (ghost_thread*)malloc(sizeof(ghost_thread));
    ghost_thread_state->ghost_thread_sync       = CreateEventA(NULL, FALSE, FALSE, NULL);
    ghost_thread_state->ghost_thread_runtime    = true;
    ghost_thread_state->ghost_thread_ready      = false;
    ghost_thread_state->windows_active          = 0;        // Ghetto reference counting.

    // Create the thread.
    CreateThread(0, 0, window_ghost_main, NULL, 0, &ghost_thread_state->ghost_thread_identifier);

    // Join the thread. In order for a window to be generated, the ghost window
    // must first be initialized on ghost thread so it can capture window creation events.
    WaitForSingleObject(ghost_thread_state->ghost_thread_sync, INFINITE);
    CloseHandle(ghost_thread_state->ghost_thread_sync);

    // Ensure that the ghost thread is ready. The return value tells us that it is.
    return (ghost_thread_state->ghost_thread_ready);

}

j5win
window_create(const char *window_name, u32 width, u32 height)
{

}

void
window_close(j5win *window_handle)
{

}