// clang-format off
// windows cares about header order
#include "windows.h"
#include "mmdeviceapi.h"
#include "mmsystem.h"
#include "endpointvolume.h"
#include "Functiondiscoverykeys_devpkey.h"
#include "PolicyConfig.h"
// clang-format on

#include "../AudioFunctions.h"

#include <cassert>
#include <codecvt>
#include <locale>

namespace {
std::string WCharPtrToString(LPCWSTR in) {
  return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t>{}
    .to_bytes(in);
}

std::wstring Utf8StrToWString(const std::string& in) {
  return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t>{}
    .from_bytes(in);
}

EDataFlow DirectionToEDataFlow(const Direction dir) {
  switch (dir) {
    case Direction::INPUT:
      return eCapture;
    case Direction::OUTPUT:
      return eRender;
  }
  __assume(0);
}

ERole RoleToERole(const Role role) {
  switch (role) {
    case Role::COMMUNICATION:
      return eCommunications;
    case Role::DEFAULT:
      return eConsole;
  }
  __assume(0);
}

IMMDevice* DeviceIDToDevice(const std::string& in) {
  IMMDeviceEnumerator* de;
  CoCreateInstance(
    __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator), (void**)&de);
  IMMDevice* device = nullptr;
  de->GetDevice(Utf8StrToWString(in).data(), &device);
  de->Release();
  return device;
}

IAudioEndpointVolume* DeviceIDToAudioEndpointVolume(
  const std::string& deviceID) {
  auto device = DeviceIDToDevice(deviceID);
  if (!device) {
    return nullptr;
  }
  IAudioEndpointVolume* volume = nullptr;
  device->Activate(
    __uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&volume);
  device->Release();
  return volume;
}
}// namespace

std::map<std::string, std::string> GetAudioDeviceList(Direction direction) {
  IMMDeviceEnumerator* de;
  CoCreateInstance(
    __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator), (void**)&de);

  IMMDeviceCollection* devices;
  de->EnumAudioEndpoints(
    DirectionToEDataFlow(direction), DEVICE_STATE_ACTIVE, &devices);

  UINT deviceCount;
  devices->GetCount(&deviceCount);
  std::map<std::string, std::string> out;

  for (UINT i = 0; i < deviceCount; ++i) {
    IMMDevice* device;
    devices->Item(i, &device);
    LPWSTR deviceID;
    device->GetId(&deviceID);
    IPropertyStore* properties;
    device->OpenPropertyStore(STGM_READ, &properties);
    PROPVARIANT name;
    properties->GetValue(PKEY_Device_FriendlyName, &name);
    out[WCharPtrToString(deviceID)] = WCharPtrToString(name.pwszVal);
    properties->Release();
    device->Release();
  }
  devices->Release();
  de->Release();
  return out;
}

std::string GetDefaultAudioDeviceID(Direction direction, Role role) {
  IMMDeviceEnumerator* de;
  CoCreateInstance(
    __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator), (void**)&de);
  IMMDevice* device;
  de->GetDefaultAudioEndpoint(
    DirectionToEDataFlow(direction), RoleToERole(role), &device);
  LPWSTR deviceID;
  device->GetId(&deviceID);
  const auto ret = WCharPtrToString(deviceID);
  device->Release();
  de->Release();
  return ret;
}

void SetDefaultAudioDeviceID(
  Direction direction,
  Role role,
  const std::string& desiredID) {
  if (desiredID == GetDefaultAudioDeviceID(direction, role)) {
    return;
  }

  IPolicyConfigVista* pPolicyConfig;

  CoCreateInstance(
    __uuidof(CPolicyConfigVistaClient), NULL, CLSCTX_ALL,
    __uuidof(IPolicyConfigVista), (LPVOID*)&pPolicyConfig);
  pPolicyConfig->SetDefaultEndpoint(
    Utf8StrToWString(desiredID).c_str(), RoleToERole(role));
  pPolicyConfig->Release();
}

bool IsAudioDeviceMuted(const std::string& deviceID) {
  auto volume = DeviceIDToAudioEndpointVolume(deviceID);
  if (!volume) {
    return false;
  }
  BOOL ret;
  volume->GetMute(&ret);
  volume->Release();
  return ret;
}

void SetIsAudioDeviceMuted(const std::string& deviceID, MuteAction action) {
  auto volume = DeviceIDToAudioEndpointVolume(deviceID);
  if (!volume) {
    return;
  }
  if (action == MuteAction::MUTE) {
    volume->SetMute(true, nullptr);
  } else if (action == MuteAction::UNMUTE) {
    volume->SetMute(false, nullptr);
  } else {
    assert(action == MuteAction::TOGGLE);
    BOOL muted;
    volume->GetMute(&muted);
    volume->SetMute(!muted, nullptr);
  }
  volume->Release();
}

namespace {
class VolumeCallback : public IAudioEndpointVolumeCallback {
 public:
  VolumeCallback(std::function<void(bool isMuted)> cb) : mCB(cb), mRefs(1) {
  }

  virtual HRESULT __stdcall QueryInterface(const IID& iid, void** ret)
    override {
    if (iid == IID_IUnknown || iid == __uuidof(IAudioEndpointVolumeCallback)) {
      *ret = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *ret = nullptr;
    return E_NOINTERFACE;
  }

  virtual ULONG __stdcall AddRef() override {
    return InterlockedIncrement(&mRefs);
  }
  virtual ULONG __stdcall Release() override {
    if (InterlockedDecrement(&mRefs) == 0) {
      delete this;
    }
    return mRefs;
  }
  virtual HRESULT OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) override {
    mCB(pNotify->bMuted);
    return S_OK;
  }

 private:
  std::function<void(bool)> mCB;
  long mRefs;
};

struct VolumeCallbackHandle {
  std::string deviceID;
  VolumeCallback* impl;
  IAudioEndpointVolume* dev;
};

}// namespace

AUDIO_DEVICE_MUTE_CALLBACK_HANDLE
AddAudioDeviceMuteUnmuteCallback(
  const std::string& deviceID,
  std::function<void(bool isMuted)> cb) {
  auto dev = DeviceIDToAudioEndpointVolume(deviceID);
  if (!dev) {
    return nullptr;
  }
  auto impl = new VolumeCallback(cb);
  auto ret = dev->RegisterControlChangeNotify(impl);
  if (ret != S_OK) {
    dev->Release();
    delete impl;
    return nullptr;
  }
  return new VolumeCallbackHandle({deviceID, impl, dev});
}

void RemoveAudioDeviceMuteUnmuteCallback(
  AUDIO_DEVICE_MUTE_CALLBACK_HANDLE _handle) {
  if (!_handle) {
    return;
  }
  const auto handle = reinterpret_cast<VolumeCallbackHandle*>(_handle);
  handle->dev->UnregisterControlChangeNotify(handle->impl);
  handle->dev->Release();
  delete handle;
  return;
}

namespace {
typedef std::function<void(Direction, Role, const std::string&)>
  DefaultChangeCallbackFun;
class DefaultChangeCallback : public IMMNotificationClient {
 public:
  DefaultChangeCallback(DefaultChangeCallbackFun cb) : mCB(cb), mRefs(1) {
  }

  virtual HRESULT __stdcall QueryInterface(const IID& iid, void** ret)
    override {
    if (iid == IID_IUnknown || iid == __uuidof(IMMNotificationClient)) {
      *ret = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *ret = nullptr;
    return E_NOINTERFACE;
  }

  virtual ULONG __stdcall AddRef() override {
    return InterlockedIncrement(&mRefs);
  }
  virtual ULONG __stdcall Release() override {
    if (InterlockedDecrement(&mRefs) == 0) {
      delete this;
    }
    return mRefs;
  }

  virtual HRESULT OnDefaultDeviceChanged(
    EDataFlow flow,
    ERole winRole,
    LPCWSTR defaultDeviceID) override {
    Role role;
    switch (winRole) {
      case ERole::eMultimedia:
        return S_OK;
      case ERole::eCommunications:
        role = Role::COMMUNICATION;
        break;
      case ERole::eConsole:
        role = Role::DEFAULT;
        break;
    }
    const Direction direction
      = (flow == EDataFlow::eCapture) ? Direction::INPUT : Direction::OUTPUT;
    mCB(direction, role, WCharPtrToString(defaultDeviceID));

    return S_OK;
  };

  virtual HRESULT OnDeviceAdded(LPCWSTR pwstrDeviceId) override {
    return S_OK;
  };

  virtual HRESULT OnDeviceRemoved(LPCWSTR pwstrDeviceId) override {
    return S_OK;
  };

  virtual HRESULT OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState)
    override {
    return S_OK;
  };

  virtual HRESULT OnPropertyValueChanged(
    LPCWSTR pwstrDeviceId,
    const PROPERTYKEY key) override {
    return S_OK;
  };

 private:
  DefaultChangeCallbackFun mCB;
  long mRefs;
};

struct DefaultChangeCallbackHandle {
  DefaultChangeCallback* impl;
  IMMDeviceEnumerator* enumerator;
};

}// namespace

DEFAULT_AUDIO_DEVICE_CHANGE_CALLBACK_HANDLE
AddDefaultAudioDeviceChangeCallback(DefaultChangeCallbackFun cb) {
  IMMDeviceEnumerator* de = nullptr;
  CoCreateInstance(
    __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator), (void**)&de);
  if (!de) {
    return nullptr;
  }
  auto impl = new DefaultChangeCallback(cb);
  if (de->RegisterEndpointNotificationCallback(impl) != S_OK) {
    de->Release();
    delete impl;
    return nullptr;
  }

  return new DefaultChangeCallbackHandle({impl, de});
}

void RemoveDefaultAudioDeviceChangeCallback(
  DEFAULT_AUDIO_DEVICE_CHANGE_CALLBACK_HANDLE _handle) {
  if (!_handle) {
    return;
  }
  return;
  const auto handle = reinterpret_cast<DefaultChangeCallbackHandle*>(_handle);
  handle->enumerator->UnregisterEndpointNotificationCallback(handle->impl);
  handle->enumerator->Release();
  delete handle;
  return;
}
