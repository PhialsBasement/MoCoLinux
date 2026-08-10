/*
 * d3dbench -- a Direct3D 11 frame loop, small enough to trust.
 *
 * The point is the fair pair. The SAME binary runs two ways under Wine:
 *
 *   WINEDLLOVERRIDES="d3d11,dxgi=n"   D3D11 -> DXVK -> Vulkan -> Venus
 *                                     -> the host's Vulkan driver -> the card
 *   WINEDLLOVERRIDES="d3d11,dxgi=b"   D3D11 -> wined3d -> OpenGL -> virgl
 *                                     -> the host's GL driver -> the card
 *
 * Same scene, same window size, same machine, one binary: the difference is
 * the stack under it and nothing else. That is the comparison this project's
 * ledger asks for, and it needs no third-party benchmark and no download.
 *
 * It clears to an animated colour and presents. There is no geometry on
 * purpose: this measures the cost of the PATH -- device creation, swapchain,
 * present, and the round trips each stack makes per frame -- not the GT 730's
 * triangle rate, which is the same card either way.
 *
 * Cross-compiled on the build host:
 *   x86_64-w64-mingw32-gcc -O2 d3dbench.c -o d3dbench.exe \
 *       -ld3d11 -ldxgi -ldxguid -municode
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <math.h>

#define WIDTH  800
#define HEIGHT 600
#define SECONDS 10

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(h, m, w, l);
}

int main(void)
{
	WNDCLASSA wc;
	HWND hwnd;
	DXGI_SWAP_CHAIN_DESC sd;
	ID3D11Device *dev = NULL;
	ID3D11DeviceContext *ctx = NULL;
	IDXGISwapChain *sc = NULL;
	ID3D11Texture2D *backbuffer = NULL;
	ID3D11RenderTargetView *rtv = NULL;
	D3D_FEATURE_LEVEL fl = 0;
	IDXGIDevice *dxgi_dev = NULL;
	IDXGIAdapter *adapter = NULL;
	DXGI_ADAPTER_DESC ad;
	LARGE_INTEGER freq, t0, now;
	unsigned long frames = 0;
	HRESULT hr;

	setvbuf(stdout, NULL, _IONBF, 0);

	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "d3dbench";
	wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
	RegisterClassA(&wc);

	hwnd = CreateWindowA("d3dbench", "d3dbench (Direct3D 11)",
			     WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			     120, 120, WIDTH, HEIGHT,
			     NULL, NULL, wc.hInstance, NULL);
	if (!hwnd) {
		printf("no window\n");
		return 1;
	}

	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 2;
	sd.BufferDesc.Width = WIDTH;
	sd.BufferDesc.Height = HEIGHT;
	sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
					   0, NULL, 0, D3D11_SDK_VERSION,
					   &sd, &sc, &dev, &fl, &ctx);
	if (FAILED(hr)) {
		printf("D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n",
		       (unsigned long)hr);
		return 1;
	}
	printf("feature level 0x%04x\n", (unsigned)fl);

	/* Which adapter the stack under us chose -- this is the line that says
	 * whether DXVK reached the real card or wined3d reached virgl. */
	if (SUCCEEDED(dev->lpVtbl->QueryInterface(dev, &IID_IDXGIDevice,
						  (void **)&dxgi_dev)) &&
	    SUCCEEDED(dxgi_dev->lpVtbl->GetAdapter(dxgi_dev, &adapter)) &&
	    SUCCEEDED(adapter->lpVtbl->GetDesc(adapter, &ad)))
		printf("adapter: %ls\n", ad.Description);

	hr = sc->lpVtbl->GetBuffer(sc, 0, &IID_ID3D11Texture2D,
				   (void **)&backbuffer);
	if (FAILED(hr)) {
		printf("GetBuffer failed: 0x%08lx\n", (unsigned long)hr);
		return 1;
	}
	hr = dev->lpVtbl->CreateRenderTargetView(dev,
			(ID3D11Resource *)backbuffer, NULL, &rtv);
	if (FAILED(hr)) {
		printf("CreateRenderTargetView failed: 0x%08lx\n",
		       (unsigned long)hr);
		return 1;
	}

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);

	for (;;) {
		MSG msg;
		float t, colour[4];

		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT)
				goto done;
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}

		QueryPerformanceCounter(&now);
		t = (float)(now.QuadPart - t0.QuadPart) / (float)freq.QuadPart;
		if (t >= SECONDS)
			break;

		colour[0] = 0.5f + 0.5f * (float)sin(t * 2.0);
		colour[1] = 0.5f + 0.5f * (float)sin(t * 2.7);
		colour[2] = 0.5f + 0.5f * (float)sin(t * 3.3);
		colour[3] = 1.0f;

		ctx->lpVtbl->OMSetRenderTargets(ctx, 1, &rtv, NULL);
		ctx->lpVtbl->ClearRenderTargetView(ctx, rtv, colour);
		sc->lpVtbl->Present(sc, 0, 0);
		frames++;
	}

done:
	QueryPerformanceCounter(&now);
	{
		double secs = (double)(now.QuadPart - t0.QuadPart) /
			      (double)freq.QuadPart;

		printf("%lu frames in %.2f s = %.0f FPS\n", frames, secs,
		       frames / secs);
	}
	return 0;
}
