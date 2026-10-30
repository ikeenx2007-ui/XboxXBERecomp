// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report,
// plus the keyboard, mapped onto one controller port by controls.ini next to the executable
#include "core/input.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------------------------------------------

// every control of the console pad a key can drive
enum Control {
    kUp, kDown, kLeft, kRight, kStart, kBack, kLeftThumb, kRightThumb,  // the 8 digital buttons, in report bit order
    kA, kB, kX, kY, kBlack, kWhite, kLeftTrigger, kRightTrigger,       // the 8 analog buttons, in report order
    kLStickUp, kLStickDown, kLStickLeft, kLStickRight,
    kRStickUp, kRStickDown, kRStickLeft, kRStickRight,
    kControlCount
};

struct ControlInfo {
    const char* name;      // the key in controls.ini
    const char* def;       // default keys
    const char* comment;   // shown in the file written on first run
};

constexpr ControlInfo kControls[kControlCount] = {
    {"DPadUp", "T", "Cruceta arriba"},
    {"DPadDown", "G", "Cruceta abajo"},
    {"DPadLeft", "F", "Cruceta izquierda"},
    {"DPadRight", "H", "Cruceta derecha"},
    {"Start", "Enter", "Start"},
    {"Back", "Backspace", "Back"},
    {"LeftThumb", "C", "Click del stick izquierdo"},
    {"RightThumb", "V", "Click del stick derecho"},
    {"A", "K, Space", "Boton A (saltar)"},
    {"B", "L", "Boton B (ki)"},
    {"X", "J", "Boton X (golpe)"},
    {"Y", "I", "Boton Y (patada)"},
    {"Black", "O", "Boton negro"},
    {"White", "P", "Boton blanco"},
    {"LeftTrigger", "Q", "Gatillo izquierdo"},
    {"RightTrigger", "E", "Gatillo derecho"},
    {"LeftStickUp", "W", "Stick izquierdo arriba"},
    {"LeftStickDown", "S", "Stick izquierdo abajo"},
    {"LeftStickLeft", "A", "Stick izquierdo izquierda"},
    {"LeftStickRight", "D", "Stick izquierdo derecha"},
    {"RightStickUp", "Up", "Stick derecho arriba"},
    {"RightStickDown", "Down", "Stick derecho abajo"},
    {"RightStickLeft", "Left", "Stick derecho izquierda"},
    {"RightStickRight", "Right", "Stick derecho derecha"},
};

struct Keyboard {
    bool enabled = true;
    int port = 0;                       // 0 to 3
    bool only_focused = true;           // ignore keys while another window is in front
    std::vector<int> keys[kControlCount];  // virtual-key codes; any of them held drives the control
};

std::once_flag g_kb_once;
Keyboard g_kb;

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// a key name from controls.ini into a virtual-key code, 0 when unknown
int KeyCode(const std::string& raw) {
    std::string k = Lower(Trim(raw));
    if (k.empty()) return 0;
    if (k.size() == 1) {
        char c = k[0];
        if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
        if (c >= '0' && c <= '9') return c;
        switch (c) {
            case '.': return VK_OEM_PERIOD;
            case '-': return VK_OEM_MINUS;
            case '+': return VK_OEM_PLUS;
        }
        return 0;
    }
    if (k[0] == 'f' && k.size() <= 3 && std::isdigit(static_cast<unsigned char>(k[1]))) {
        int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    if (k.rfind("numpad", 0) == 0 && k.size() == 7 && std::isdigit(static_cast<unsigned char>(k[6])))
        return VK_NUMPAD0 + (k[6] - '0');
    static const std::pair<const char*, int> kNames[] = {
        {"enter", VK_RETURN},      {"return", VK_RETURN},    {"space", VK_SPACE},     {"espacio", VK_SPACE},
        {"backspace", VK_BACK},    {"tab", VK_TAB},          {"esc", VK_ESCAPE},      {"escape", VK_ESCAPE},
        {"shift", VK_SHIFT},       {"lshift", VK_LSHIFT},    {"rshift", VK_RSHIFT},   {"ctrl", VK_CONTROL},
        {"lctrl", VK_LCONTROL},    {"rctrl", VK_RCONTROL},   {"alt", VK_MENU},        {"lalt", VK_LMENU},
        {"ralt", VK_RMENU},        {"up", VK_UP},            {"down", VK_DOWN},       {"left", VK_LEFT},
        {"right", VK_RIGHT},       {"arriba", VK_UP},        {"abajo", VK_DOWN},      {"izquierda", VK_LEFT},
        {"derecha", VK_RIGHT},     {"insert", VK_INSERT},    {"delete", VK_DELETE},   {"home", VK_HOME},
        {"end", VK_END},           {"pageup", VK_PRIOR},     {"pagedown", VK_NEXT},   {"capslock", VK_CAPITAL},
        {"mouseleft", VK_LBUTTON}, {"mouseright", VK_RBUTTON}, {"mousemiddle", VK_MBUTTON},
    };
    for (auto [name, code] : kNames)
        if (k == name) return code;
    return 0;
}

std::vector<int> ParseKeys(const std::string& list, const char* control) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? list.size() + 1 : comma + 1;
        if (Trim(item).empty()) continue;
        if (int code = KeyCode(item)) out.push_back(code);
        else LOGW("input: controls.ini: unknown key \"%s\" for %s", Trim(item).c_str(), control);
    }
    return out;
}

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return std::filesystem::current_path();
    return std::filesystem::path(buf).parent_path();
}

void WriteDefaults(const std::filesystem::path& file) {
    std::ofstream f(file);
    if (!f) return;
    f << "; Controles de teclado. Cambia la tecla despues del '=' y volve a abrir el juego.\n"
         "; Podes poner varias teclas separadas por coma, por ejemplo: A = J, Space\n"
         "; Nombres validos: letras (A-Z), numeros (0-9), F1-F12, Enter, Space, Backspace, Tab, Esc,\n"
         ";   Shift, LShift, RShift, Ctrl, LCtrl, RCtrl, Alt, Up, Down, Left, Right, Numpad0-Numpad9,\n"
         ";   Insert, Delete, Home, End, PageUp, PageDown, MouseLeft, MouseRight, MouseMiddle\n"
         "; Dejar una tecla vacia la desactiva.\n\n"
         "[Teclado]\n"
         "; 1 = activado, 0 = desactivado\n"
         "Enabled = 1\n"
         "; En que control (1 a 4) actua el teclado. Si hay un joystick en ese mismo puerto, funcionan los dos.\n"
         "Port = 1\n"
         "; 1 = solo cuando la ventana del juego esta al frente\n"
         "OnlyWhenFocused = 1\n\n"
         "[Teclas]\n";
    for (const ControlInfo& c : kControls) f << "; " << c.comment << "\n" << c.name << " = " << c.def << "\n";
}

void LoadKeyboard() {
    for (int i = 0; i < kControlCount; i++) g_kb.keys[i] = ParseKeys(kControls[i].def, kControls[i].name);
    std::filesystem::path file = ExeDir() / "controls.ini";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        WriteDefaults(file);
        LOGI("input: keyboard uses the default keys, wrote %s", file.string().c_str());
        return;
    }
    std::ifstream f(file);
    if (!f) {
        LOGW("input: could not read %s, keyboard uses the default keys", file.string().c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string value = Trim(line.substr(eq + 1));
        if (key == "enabled") g_kb.enabled = value != "0";
        else if (key == "port") g_kb.port = std::clamp(std::atoi(value.c_str()), 1, 4) - 1;
        else if (key == "onlywhenfocused") g_kb.only_focused = value != "0";
        else {
            bool known = false;
            for (int i = 0; i < kControlCount && !known; i++)
                if (key == Lower(kControls[i].name)) {
                    g_kb.keys[i] = ParseKeys(value, kControls[i].name);
                    known = true;
                }
            if (!known) LOGW("input: controls.ini: unknown setting \"%s\"", key.c_str());
        }
    }
    LOGI("input: keyboard %s on controller %d, keys from %s", g_kb.enabled ? "enabled" : "disabled", g_kb.port + 1,
         file.string().c_str());
}

const Keyboard& Kb() {
    std::call_once(g_kb_once, LoadKeyboard);
    return g_kb;
}

bool GameInFront() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

// which controls the keyboard holds right now, one bit per Control
uint32_t KeyboardHeld() {
    const Keyboard& kb = Kb();
    if (!kb.enabled || (kb.only_focused && !GameInFront())) return 0;
    uint32_t held = 0;
    for (int i = 0; i < kControlCount; i++)
        for (int code : kb.keys[i])
            if (GetAsyncKeyState(code) & 0x8000) {
                held |= 1u << i;
                break;
            }
    return held;
}

int16_t StickAxis(uint32_t held, Control positive, Control negative) {
    bool p = held & (1u << positive), n = held & (1u << negative);
    if (p == n) return 0;
    return p ? int16_t(32767) : int16_t(-32768);
}

// the keyboard's controls onto a port's report; a pad on the same port keeps working
void MergeKeyboard(Pad& p) {
    uint32_t held = KeyboardHeld();
    p.connected = true;  // the port has a controller even with no pad plugged in
    if (!held) return;
    for (int i = 0; i < 8; i++)
        if (held & (1u << (kUp + i))) p.buttons |= uint16_t(1u << i);
    for (int i = 0; i < 8; i++)
        if (held & (1u << (kA + i))) p.analog[i] = 0xFF;
    // a stick key wins over the pad's stick on that axis
    if (int16_t v = StickAxis(held, kLStickRight, kLStickLeft)) p.lx = v;
    if (int16_t v = StickAxis(held, kLStickUp, kLStickDown)) p.ly = v;  // the report has y up
    if (int16_t v = StickAxis(held, kRStickRight, kRStickLeft)) p.rx = v;
    if (int16_t v = StickAxis(held, kRStickUp, kRStickDown)) p.ry = v;
}

}  // namespace

void Init() {
    Kb();  // load controls.ini now, so the log shows it at startup
    if (g_sdl) return;  // started already for the game code window
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }
    const Keyboard& kb = Kb();
    if (kb.enabled && port == kb.port) MergeKeyboard(p);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    uint16_t held = 0;
    // the keyboard's A B X Y count too
    uint32_t kb = KeyboardHeld();
    constexpr std::pair<Control, uint16_t> kKeyFace[] = {{kA, 0x1000}, {kB, 0x2000}, {kX, 0x4000}, {kY, 0x8000}};
    for (auto [control, mask] : kKeyFace)
        if (kb & (1u << control)) held |= mask;
    std::lock_guard lock(g_lock);
    UpdatePads();
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    return held;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
