#include "wedecode/drop_target.h"

#include "app_window.h"

#include <shellapi.h>

#include <filesystem>
#include <vector>

namespace wedecode {

FolderDropTarget::FolderDropTarget(AppWindow& owner) : owner_(owner) {}

HRESULT STDMETHODCALLTYPE FolderDropTarget::QueryInterface(REFIID iid, void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (iid == IID_IUnknown || iid == IID_IDropTarget) {
    *object = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FolderDropTarget::AddRef() {
  return static_cast<ULONG>(InterlockedIncrement(&references_));
}

ULONG STDMETHODCALLTYPE FolderDropTarget::Release() {
  const auto references = static_cast<ULONG>(InterlockedDecrement(&references_));
  if (references == 0) delete this;
  return references;
}

bool FolderDropTarget::has_file_drop(IDataObject* data_object) {
  if (!data_object) return false;
  FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  return data_object->QueryGetData(&format) == S_OK;
}

HRESULT STDMETHODCALLTYPE FolderDropTarget::DragEnter(IDataObject* data_object, DWORD, POINTL, DWORD* effect) {
  if (!effect) return E_POINTER;
  const bool accepted = has_file_drop(data_object);
  *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  owner_.set_drop_hover(accepted);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FolderDropTarget::DragOver(DWORD, POINTL, DWORD* effect) {
  if (!effect) return E_POINTER;
  *effect = DROPEFFECT_COPY;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FolderDropTarget::DragLeave() {
  owner_.set_drop_hover(false);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FolderDropTarget::Drop(IDataObject* data_object, DWORD, POINTL, DWORD* effect) {
  if (!effect) return E_POINTER;
  *effect = DROPEFFECT_NONE;
  owner_.set_drop_hover(false);
  if (!has_file_drop(data_object)) return S_OK;

  FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data_object->GetData(&format, &medium))) return S_OK;

  std::vector<std::filesystem::path> paths;
  const auto drop = static_cast<HDROP>(medium.hGlobal);
  const auto count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
  for (UINT index = 0; index < count; ++index) {
    const auto length = DragQueryFileW(drop, index, nullptr, 0);
    if (length == 0) continue;
    std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
    DragQueryFileW(drop, index, path.data(), length + 1);
    path.resize(length);
    paths.emplace_back(std::move(path));
  }
  ReleaseStgMedium(&medium);

  if (!paths.empty()) {
    owner_.add_dropped_folders(paths);
    *effect = DROPEFFECT_COPY;
  }
  return S_OK;
}

}  // namespace wedecode


