// GI1: SDL3 C-API stubs for the iOS static-GS build.
//
// The app links SDL2-for-iOS (via raylib), so PCSX2's own libSDL3.a must NOT
// link: its UIKit ObjC classes collide with SDL2's and the app dies at launch
// (duplicate SDL_LifecycleObserver). But libPCSX2.a pulls SDLInputSource,
// SDLAudioStream and usb-pad FF objects through the GS -> VMManager ->
// InputManager/AudioStream chain, and those reference the SDL3 C API.
//
// These definitions satisfy those references. They are never called on the GS
// path (the runner owns pad/audio; PCSX2 input sources are never reloaded and
// the SDL audio backend is never selected), so every stub returns a benign
// failure value. Signatures mirror SDL3's headers (opaque structs, Uint IDs,
// plain-int enums); see 3rdparty SDL3 SDL_gamepad/joystick/haptic/audio/log.h.
//
// If GS ever needs real PCSX2 input/audio on iOS, the correct fix is to stop
// building the SDL sources into libPCSX2.a (exclude Input/SDLInputSource.cpp,
// Host/SDLAudioStream.cpp, USB/usb-pad/usb-pad-sdl-ff.cpp in ios-platform and
// stub the 4 remaining C++ symbols), not to link libSDL3.a.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {

typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Haptic SDL_Haptic;
typedef struct SDL_HapticEffect SDL_HapticEffect;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef struct SDL_AudioSpec SDL_AudioSpec;
typedef struct SDL_GamepadBinding SDL_GamepadBinding;
typedef uint32_t SDL_HapticEffectID;
typedef uint32_t SDL_AudioDeviceID;
typedef uint32_t SDL_JoystickID;
typedef uint32_t SDL_PropertiesID;
/* Plain-int enums in SDL3: SDL_GamepadButtonLabel, SDL_GamepadButton,
   SDL_GamepadType, SDL_LogPriority. */
typedef void (*GI1_AudioStreamCallback)(void* userdata, SDL_AudioStream* stream,
                                       int additional_amount, int total_amount);
typedef void (*GI1_LogOutputFunction)(void* userdata, int category, int priority,
                                     const char* message);

void SDL_CloseGamepad(SDL_Gamepad* gamepad) { (void)gamepad; }
void SDL_CloseHaptic(SDL_Haptic* haptic) { (void)haptic; }
void SDL_CloseJoystick(SDL_Joystick* joystick) { (void)joystick; }
SDL_HapticEffectID SDL_CreateHapticEffect(SDL_Haptic* haptic, const SDL_HapticEffect* effect)
{
  (void)haptic;
  (void)effect;
  return 0;
}
void SDL_DestroyAudioStream(SDL_AudioStream* stream) { (void)stream; }
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec* spec, int* sample_frames)
{
  (void)devid;
  (void)spec;
  (void)sample_frames;
  return false;
}
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream* stream)
{
  (void)stream;
  return 0;
}
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char* name, bool default_value)
{
  (void)props;
  (void)name;
  return default_value;
}
SDL_GamepadBinding** SDL_GetGamepadBindings(SDL_Gamepad* gamepad, int* count)
{
  (void)gamepad;
  if (count)
    *count = 0;
  return NULL;
}
int SDL_GetGamepadButtonLabel(SDL_Gamepad* gamepad, int button)
{
  (void)gamepad;
  (void)button;
  return 0;
}
SDL_Joystick* SDL_GetGamepadJoystick(SDL_Gamepad* gamepad)
{
  (void)gamepad;
  return NULL;
}
char** SDL_GetGamepadMappings(int* count)
{
  if (count)
    *count = 0;
  return NULL;
}
const char* SDL_GetGamepadName(SDL_Gamepad* gamepad)
{
  (void)gamepad;
  return NULL;
}
int SDL_GetGamepadPlayerIndex(SDL_Gamepad* gamepad)
{
  (void)gamepad;
  return -1;
}
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad* gamepad)
{
  (void)gamepad;
  return 0;
}
SDL_JoystickID SDL_GetJoystickID(SDL_Joystick* joystick)
{
  (void)joystick;
  return 0;
}
const char* SDL_GetJoystickName(SDL_Joystick* joystick)
{
  (void)joystick;
  return NULL;
}
int SDL_GetJoystickPlayerIndex(SDL_Joystick* joystick)
{
  (void)joystick;
  return -1;
}
int SDL_GetNumJoystickAxes(SDL_Joystick* joystick)
{
  (void)joystick;
  return 0;
}
int SDL_GetNumJoystickButtons(SDL_Joystick* joystick)
{
  (void)joystick;
  return 0;
}
int SDL_GetNumJoystickHats(SDL_Joystick* joystick)
{
  (void)joystick;
  return 0;
}
int SDL_GetRealGamepadType(SDL_Gamepad* gamepad)
{
  (void)gamepad;
  return 0;
}
bool SDL_InitHapticRumble(SDL_Haptic* haptic)
{
  (void)haptic;
  return false;
}
bool SDL_IsGamepad(SDL_JoystickID instance_id)
{
  (void)instance_id;
  return false;
}
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec,
                                           GI1_AudioStreamCallback callback, void* userdata)
{
  (void)devid;
  (void)spec;
  (void)callback;
  (void)userdata;
  return NULL;
}
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID instance_id)
{
  (void)instance_id;
  return NULL;
}
SDL_Haptic* SDL_OpenHapticFromJoystick(SDL_Joystick* joystick)
{
  (void)joystick;
  return NULL;
}
SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID instance_id)
{
  (void)instance_id;
  return NULL;
}
bool SDL_PlayHapticRumble(SDL_Haptic* haptic, float strength, uint32_t length)
{
  (void)haptic;
  (void)strength;
  (void)length;
  return false;
}
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len)
{
  (void)stream;
  (void)buf;
  (void)len;
  return false;
}
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID devid)
{
  (void)devid;
  return false;
}
bool SDL_RumbleGamepad(SDL_Gamepad* gamepad, uint16_t low_frequency_rumble,
                       uint16_t high_frequency_rumble, uint32_t duration_ms)
{
  (void)gamepad;
  (void)low_frequency_rumble;
  (void)high_frequency_rumble;
  (void)duration_ms;
  return false;
}
bool SDL_RunHapticEffect(SDL_Haptic* haptic, SDL_HapticEffectID effect, uint32_t iterations)
{
  (void)haptic;
  (void)effect;
  (void)iterations;
  return false;
}
bool SDL_SetGamepadLED(SDL_Gamepad* gamepad, uint8_t red, uint8_t green, uint8_t blue)
{
  (void)gamepad;
  (void)red;
  (void)green;
  (void)blue;
  return false;
}
void SDL_SetLogOutputFunction(GI1_LogOutputFunction callback, void* userdata)
{
  (void)callback;
  (void)userdata;
}
void SDL_SetLogPriorities(int priority)
{
  (void)priority;
}
bool SDL_StopHapticEffect(SDL_Haptic* haptic, SDL_HapticEffectID effect)
{
  (void)haptic;
  (void)effect;
  return false;
}
bool SDL_StopHapticRumble(SDL_Haptic* haptic)
{
  (void)haptic;
  return false;
}
bool SDL_UpdateHapticEffect(SDL_Haptic* haptic, SDL_HapticEffectID effect,
                            const SDL_HapticEffect* data)
{
  (void)haptic;
  (void)effect;
  (void)data;
  return false;
}

// IB3: the OM1 island's link pulls usb-pad-sdl-ff.cpp.o (USB force-feedback),
// which references three more haptic functions than the GS closure does.
// Force-feedback is unreachable on the offline path (the island never creates
// USB devices or pads), so these abort if hit: a call would prove the no-USB
// subset wrong, the way OM1StubHost's hwIntcIrq does.
void SDL_DestroyHapticEffect(SDL_Haptic* haptic, SDL_HapticEffectID effect)
{
  (void)haptic;
  (void)effect;
  std::fprintf(stderr, "[om1] fatal: SDL_DestroyHapticEffect in no-USB subset\n");
  std::abort();
}
uint32_t SDL_GetHapticFeatures(SDL_Haptic* haptic)
{
  (void)haptic;
  std::fprintf(stderr, "[om1] fatal: SDL_GetHapticFeatures in no-USB subset\n");
  std::abort();
}
bool SDL_SetHapticAutocenter(SDL_Haptic* haptic, int autocenter)
{
  (void)haptic;
  (void)autocenter;
  std::fprintf(stderr, "[om1] fatal: SDL_SetHapticAutocenter in no-USB subset\n");
  std::abort();
}

} // extern "C"
