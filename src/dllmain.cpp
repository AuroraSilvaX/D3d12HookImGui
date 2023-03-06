#include "hook.hpp"
#include <Windows.h>
#include <fstream>
#include <utility>

DWORD WINAPI AttachThread(LPVOID lParam) {

  Init();

  InstallHooks();

  return 0;
}

DWORD WINAPI DetachThread(LPVOID lParam) {

  RemoveHooks();

  return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
  case DLL_PROCESS_ATTACH: {

    CreateThread(nullptr, 0, &AttachThread, static_cast<LPVOID>(hModule), 0,
                 nullptr);
    break;
  }
  case DLL_PROCESS_DETACH: {
    CreateThread(nullptr, 0, &DetachThread, static_cast<LPVOID>(hModule), 0,
                 nullptr);
    break;
  }
  }
  return TRUE;
}
