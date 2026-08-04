#pragma once

#include <windows.h>
#include <oleidl.h>

namespace wedecode {

class AppWindow;

class FolderDropTarget final : public IDropTarget {
public:
  explicit FolderDropTarget(AppWindow& owner);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data_object, DWORD key_state, POINTL point, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL point, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragLeave() override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data_object, DWORD key_state, POINTL point, DWORD* effect) override;

private:
  ~FolderDropTarget() = default;
  long references_{1};
  AppWindow& owner_;

  static bool has_file_drop(IDataObject* data_object);
};

}  // namespace wedecode


