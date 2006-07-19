// DShowVRM9Allocator.cpp
// KlayGE DirectShow VRM9分配器类 实现文件
// Ver 3.4.0
// 版权所有(C) 龚敏敏, 2006
// Homepage: http://klayge.sourceforge.net
//
// 3.4.0
// 初次建立 (2006.7.15)
//
// 修改记录
/////////////////////////////////////////////////////////////////////

#include <KlayGE/KlayGE.hpp>
#include <KlayGE/ThrowErr.hpp>
#include <KlayGE/COMPtr.hpp>
#include <KlayGE/ElementFormat.hpp>
#include <KlayGE/Context.hpp>
#include <KlayGE/Texture.hpp>
#include <KlayGE/RenderFactory.hpp>

#include <d3d9.h>
#include <boost/assert.hpp>
#include <boost/pool/pool_alloc.hpp>

#include <KlayGE/D3D9/D3D9Mapping.hpp>
#include <KlayGE/D3D9/D3D9Texture.hpp>
#include <KlayGE/DShow/DShowVMR9Allocator.hpp>

namespace KlayGE
{
	DShowVMR9Allocator::DShowVMR9Allocator(HWND wnd)
					: ref_count_(1),
						wnd_(wnd)
	{
		d3d_ = MakeCOMPtr(::Direct3DCreate9(D3D_SDK_VERSION));

		D3DDISPLAYMODE dm;
		d3d_->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm);

		D3DPRESENT_PARAMETERS pp;
		memset(&pp, 0, sizeof(pp));
		pp.Windowed = true;
		pp.hDeviceWindow = wnd_;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

		IDirect3DDevice9* d3d_device;
		d3d_->CreateDevice(D3DADAPTER_DEFAULT,
								D3DDEVTYPE_HAL,
								wnd_,
								D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_MULTITHREADED,
								&pp,
								&d3d_device);
		d3d_device_ = MakeCOMPtr(d3d_device);
	}

	DShowVMR9Allocator::~DShowVMR9Allocator()
	{
		this->DeleteSurfaces();
	}

	void DShowVMR9Allocator::DeleteSurfaces()
	{
		boost::mutex::scoped_lock lock(mutex_);

		// clear out the private texture
		cache_tex_.reset();
		cache_surf_.reset();
		private_tex_.reset();

		for (size_t i = 0; i < surfaces_.size(); ++ i) 
		{
			if (surfaces_[i] != NULL)
			{
				surfaces_[i]->Release();
				surfaces_[i] = NULL;
			}
		}
	}


	//IVMRSurfaceAllocator9
	HRESULT DShowVMR9Allocator::InitializeDevice(DWORD_PTR dwUserID,
				VMR9AllocationInfo* lpAllocInfo, DWORD* lpNumBuffers)
	{
		if (dwUserID != USER_ID)
		{
			return S_OK;
		}

		if (NULL == lpNumBuffers)
		{
			return E_POINTER;
		}

		if (!vmr_surf_alloc_notify_)
		{
			return E_FAIL;
		}

		HRESULT hr = S_OK;

		// NOTE:
		// we need to make sure that we create textures because
		// surfaces can not be textured onto a primitive.
		lpAllocInfo->dwFlags |= VMR9AllocFlag_TextureSurface;

		this->DeleteSurfaces();
		surfaces_.resize(*lpNumBuffers);
		hr = vmr_surf_alloc_notify_->AllocateSurfaceHelper(lpAllocInfo, lpNumBuffers, &surfaces_[0]);

		// If we couldn't create a texture surface and 
		// the format is not an alpha format,
		// then we probably cannot create a texture.
		// So what we need to do is create a private texture
		// and copy the decoded images onto it.
		if (FAILED(hr) && !(lpAllocInfo->dwFlags & VMR9AllocFlag_3DRenderTarget))
		{
			this->DeleteSurfaces();            

			// is surface YUV ?
			if (lpAllocInfo->Format > '0000') 
			{           
				D3DDISPLAYMODE dm; 
				TIF(d3d_device_->GetDisplayMode(NULL, &dm));

				// create the private texture
				IDirect3DTexture9* tex;
				TIF(d3d_device_->CreateTexture(lpAllocInfo->dwWidth, lpAllocInfo->dwHeight,
										1, 
										D3DUSAGE_RENDERTARGET, 
										dm.Format, 
										D3DPOOL_DEFAULT,
										&tex, NULL));
				private_tex_ = MakeCOMPtr(tex);
			}

			lpAllocInfo->dwFlags &= ~VMR9AllocFlag_TextureSurface;
			lpAllocInfo->dwFlags |= VMR9AllocFlag_OffscreenSurface;

			TIF(vmr_surf_alloc_notify_->AllocateSurfaceHelper(lpAllocInfo, lpNumBuffers, &surfaces_[0]));
		}


		RenderFactory& rf = Context::Instance().RenderFactoryInstance();
		present_tex_ = rf.MakeTexture2D(lpAllocInfo->dwWidth, lpAllocInfo->dwHeight, 1, EF_ARGB8);
		present_tex_->Usage(Texture::TU_RenderTarget);

		IDirect3DSurface9* surf;
		d3d_device_->CreateOffscreenPlainSurface(lpAllocInfo->dwWidth, lpAllocInfo->dwHeight,
			D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &surf, NULL);
		cache_surf_ = MakeCOMPtr(surf);

		return S_OK;
	}
	            
	HRESULT DShowVMR9Allocator::TerminateDevice(DWORD_PTR dwID)
	{
		if (dwID != USER_ID)
		{
			return S_OK;
		}

		this->DeleteSurfaces();
		return S_OK;
	}
	    
	HRESULT DShowVMR9Allocator::GetSurface(DWORD_PTR dwUserID,
			DWORD SurfaceIndex, DWORD /*SurfaceFlags*/, IDirect3DSurface9** lplpSurface)
	{
		if (dwUserID != USER_ID)
		{
			*lplpSurface = NULL;
			return S_OK;
		}

		if (NULL == lplpSurface)
		{
			return E_POINTER;
		}

		if (SurfaceIndex >= surfaces_.size()) 
		{
			return E_FAIL;
		}

		boost::mutex::scoped_lock lock(mutex_);

		*lplpSurface = surfaces_[SurfaceIndex];
		(*lplpSurface)->AddRef();
		return S_OK;
	}
	    
	HRESULT DShowVMR9Allocator::AdviseNotify(IVMRSurfaceAllocatorNotify9* lpIVMRSurfAllocNotify)
	{
		boost::mutex::scoped_lock lock(mutex_);

		vmr_surf_alloc_notify_ = MakeCOMPtr(lpIVMRSurfAllocNotify);
		vmr_surf_alloc_notify_->AddRef();

		HMONITOR hMonitor = d3d_->GetAdapterMonitor(D3DADAPTER_DEFAULT);
		TIF(vmr_surf_alloc_notify_->SetD3DDevice(d3d_device_.get(), hMonitor));

		return S_OK;
	}

	HRESULT DShowVMR9Allocator::StartPresenting(DWORD_PTR dwUserID)
	{
		if (dwUserID != USER_ID)
		{
			return S_OK;
		}

		boost::mutex::scoped_lock lock(mutex_);

		if (!d3d_device_)
		{
			return E_FAIL;
		}

		return S_OK;
	}

	HRESULT DShowVMR9Allocator::StopPresenting(DWORD_PTR dwUserID)
	{
		if (dwUserID != USER_ID)
		{
			return S_OK;
		}

		return S_OK;
	}

	HRESULT DShowVMR9Allocator::PresentImage(DWORD_PTR dwUserID, VMR9PresentationInfo* lpPresInfo)
	{
		if (dwUserID != USER_ID)
		{
			return S_OK;
		}

		// parameter validation
		if (NULL == lpPresInfo)
		{
			return E_POINTER;
		}
		else
		{
			if (NULL == lpPresInfo->lpSurf)
			{
				return E_POINTER;
			}
		}

		boost::mutex::scoped_lock lock(mutex_);

		// if we created a  private texture
		// blt the decoded image onto the texture.
		if (private_tex_)
		{   
			ID3D9SurfacePtr surface;
			{
				IDirect3DSurface9* tmp;
				TIF(private_tex_->GetSurfaceLevel(0 , &tmp));
				surface = MakeCOMPtr(tmp);
			}

			// copy the full surface onto the texture's surface
			TIF(d3d_device_->StretchRect(lpPresInfo->lpSurf, NULL,
								 surface.get(), NULL,
								 D3DTEXF_NONE));

			cache_tex_ = private_tex_;
		}
		else
		{
			// this is the case where we have got the textures allocated by VMR
			// all we need to do is to get them from the surface

			IDirect3DTexture9* tmp;
			TIF(lpPresInfo->lpSurf->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&tmp)));
			cache_tex_ = MakeCOMPtr(tmp);
		}

		return S_OK;
	}

	// IUnknown
	HRESULT DShowVMR9Allocator::QueryInterface(REFIID riid, void** ppvObject)
	{
		HRESULT hr = E_NOINTERFACE;

		if (NULL == ppvObject)
		{
			hr = E_POINTER;
		} 
		else
		{
			if (IID_IVMRSurfaceAllocator9 == riid)
			{
				*ppvObject = static_cast<IVMRSurfaceAllocator9*>(this);
				this->AddRef();
				hr = S_OK;
			} 
			else
			{
				if (IID_IVMRImagePresenter9 == riid)
				{
					*ppvObject = static_cast<IVMRImagePresenter9*>(this);
					this->AddRef();
					hr = S_OK;
				} 
				else
				{
					if (IID_IUnknown == riid)
					{
						*ppvObject = static_cast<IUnknown*>(static_cast<IVMRSurfaceAllocator9*>(this));
						this->AddRef();
						hr = S_OK;    
					}
				}
			}
		}

		return hr;
	}

	ULONG DShowVMR9Allocator::AddRef()
	{
		return ::InterlockedIncrement(&ref_count_);
	}

	ULONG DShowVMR9Allocator::Release()
	{
		ULONG ret = ::InterlockedDecrement(&ref_count_);
		if (0 == ret)
		{
			delete this;
		}

		return ret;
	}

	TexturePtr DShowVMR9Allocator::PresentTexture()
	{
		boost::mutex::scoped_lock lock(mutex_);

		if (cache_tex_)
		{
			{
				IDirect3DSurface9* tmp;
				TIF(cache_tex_->GetSurfaceLevel(0 , &tmp));
				ID3D9SurfacePtr surf = MakeCOMPtr(tmp);

				TIF(d3d_device_->GetRenderTargetData(surf.get(), cache_surf_.get()));
			}

			D3DSURFACE_DESC desc;
			cache_tex_->GetLevelDesc(0, &desc);

			ElementFormat const ef = D3D9Mapping::MappingFormat(desc.Format);
			uint32_t const line_size = desc.Width * NumFormatBytes(ef);
			std::vector<uint8_t, boost::pool_allocator<uint8_t> > data(line_size * desc.Height);

			D3DLOCKED_RECT d3dlocked_rc;
			cache_surf_->LockRect(&d3dlocked_rc, NULL, D3DLOCK_NOSYSLOCK | D3DLOCK_READONLY);
			uint8_t const * src = static_cast<uint8_t const *>(d3dlocked_rc.pBits);
			uint8_t* dst = &data[0];
			for (uint32_t y = 0; y < desc.Height; ++ y)
			{
				std::copy(src, src + line_size, dst);
				src += d3dlocked_rc.Pitch;
				dst += line_size;
			}
			cache_surf_->UnlockRect();

			present_tex_->CopyMemoryToTexture2D(0, &data[0], ef,
				present_tex_->Width(0), present_tex_->Height(0), 0, 0, desc.Width, desc.Height);
		}

		return present_tex_;
	}
}
