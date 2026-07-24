#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <deque>
#include <string>
#include <algorithm>
#include "../shared/rgpu_phase0_protocol.h"
using namespace rgpu::phase0;

struct Message { Header h{}; std::vector<uint8_t> payload; };
struct Ring {
  std::deque<Message> q;
  Status push(Message m){ if(q.size()>=kQueueSlots) return Status::QueueFull; q.push_back(std::move(m)); return Status::Ok; }
  bool pop(Message& m){ if(q.empty()) return false; m=std::move(q.front()); q.pop_front(); return true; }
};
struct Executor {
  uint32_t w=0,h=0,stride=0; std::vector<uint32_t> pixels; uint64_t completed=0;
  Status execute(const Message& m){
    if(!valid_header(m.h) || m.h.bytes!=m.payload.size()) return Status::BadMessage;
    if(m.h.opcode==Opcode::CreateSurface){ if(m.payload.size()!=sizeof(SurfaceDesc)) return Status::BadMessage; auto d=*reinterpret_cast<const SurfaceDesc*>(m.payload.data()); if(!d.width||!d.height||d.width>4096||d.height>4096||d.stride<d.width*4) return Status::BadMessage; w=d.width;h=d.height;stride=d.stride; pixels.assign(size_t(w)*h,0); }
    else if(m.h.opcode==Opcode::ClearSurface){ if(m.payload.size()!=sizeof(ClearPayload)||pixels.empty()) return Status::BadMessage; auto p=*reinterpret_cast<const ClearPayload*>(m.payload.data()); std::fill(pixels.begin(),pixels.end(),p.bgra); }
    else if(m.h.opcode==Opcode::Present){ if(m.payload.size()!=sizeof(PresentPayload)||pixels.empty()) return Status::BadMessage; auto p=*reinterpret_cast<const PresentPayload*>(m.payload.data()); completed=std::max(completed,p.fence_value); }
    else return Status::BadMessage;
    return Status::Ok;
  }
};
static uint32_t crc32(const uint8_t* p,size_t n){ uint32_t c=0xffffffffu; for(size_t i=0;i<n;i++){ c^=p[i]; for(int b=0;b<8;b++) c=(c>>1)^(0xedb88320u&uint32_t(-(int)(c&1))); } return ~c; }
static Message make_msg(Opcode op,uint64_t seq,const void* p,uint32_t n){ Message m; m.h={kMagic,kMajor,kMinor,n,seq,op,0}; m.payload.resize(n); if(n) memcpy(m.payload.data(),p,n); return m; }
static int selftest(){
  Ring ring; Executor ex; uint64_t seq=1;
  SurfaceDesc d{320,180,320*4,1}; if(ring.push(make_msg(Opcode::CreateSurface,seq++,&d,sizeof(d)))!=Status::Ok) return 10;
  ClearPayload c{1,0xff336699u}; if(ring.push(make_msg(Opcode::ClearSurface,seq++,&c,sizeof(c)))!=Status::Ok) return 11;
  PresentPayload p{1,7}; if(ring.push(make_msg(Opcode::Present,seq++,&p,sizeof(p)))!=Status::Ok) return 12;
  Message m; while(ring.pop(m)){ if(ex.execute(m)!=Status::Ok) return 13; }
  const uint32_t got=crc32(reinterpret_cast<const uint8_t*>(ex.pixels.data()),ex.pixels.size()*4);
  std::vector<uint32_t> expected(size_t(d.width)*d.height,c.bgra); const uint32_t want=crc32(reinterpret_cast<const uint8_t*>(expected.data()),expected.size()*4);
  Ring saturated; for(uint32_t i=0;i<kQueueSlots;i++) if(saturated.push(make_msg(Opcode::Hello,i,nullptr,0))!=Status::Ok) return 14; if(saturated.push(make_msg(Opcode::Hello,99,nullptr,0))!=Status::QueueFull) return 15;
  Header bad{kMagic+1,kMajor,kMinor,0,1,Opcode::Hello,0}; Message bm; bm.h=bad; if(ex.execute(bm)!=Status::BadMessage) return 16;
  std::printf("PHASE0_SELFTEST=PASS width=%u height=%u crc32=%08x fence=%llu queue_slots=%u\n",ex.w,ex.h,got,(unsigned long long)ex.completed,kQueueSlots);
  return got==want && ex.completed==7 ? 0 : 17;
}
static Executor g_ex;
static LRESULT CALLBACK wndproc(HWND h,UINT msg,WPARAM w,LPARAM l){
 if(msg==WM_DESTROY){PostQuitMessage(0);return 0;} if(msg==WM_PAINT){PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); if(!g_ex.pixels.empty()){BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=(LONG)g_ex.w;bi.bmiHeader.biHeight=-(LONG)g_ex.h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;StretchDIBits(dc,0,0,g_ex.w,g_ex.h,0,0,g_ex.w,g_ex.h,g_ex.pixels.data(),&bi,DIB_RGB_COLORS,SRCCOPY);} EndPaint(h,&ps);return 0;} return DefWindowProcW(h,msg,w,l);
}
static int windowproof(HINSTANCE hi){
 WNDCLASSW wc{};wc.lpfnWndProc=wndproc;wc.hInstance=hi;wc.lpszClassName=L"RemoteGpuPhase0";wc.hCursor=LoadCursor(nullptr,IDC_ARROW); if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 20;
 HWND h=CreateWindowW(wc.lpszClassName,L"RemoteGPU Phase 0 returned-frame presentation proof",WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,800,500,nullptr,nullptr,hi,nullptr); if(!h)return 21;
 g_ex.w=640;g_ex.h=360;g_ex.stride=2560;g_ex.pixels.resize(size_t(g_ex.w)*g_ex.h); MSG msg{}; uint32_t frame=0; while(true){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)return 0;TranslateMessage(&msg);DispatchMessageW(&msg);} for(uint32_t y=0;y<g_ex.h;y++)for(uint32_t x=0;x<g_ex.w;x++){uint8_t b=(uint8_t)(x+frame),gg=(uint8_t)(y+frame),r=(uint8_t)(x+y+frame);g_ex.pixels[size_t(y)*g_ex.w+x]=0xff000000u|(uint32_t(r)<<16)|(uint32_t(gg)<<8)|b;} InvalidateRect(h,nullptr,FALSE); Sleep(16); if(++frame>600) DestroyWindow(h);}
}
int WINAPI wWinMain(HINSTANCE h,HINSTANCE,LPWSTR cmd,int){ if(wcsstr(cmd,L"--window")) return windowproof(h); return selftest(); }
int main(int argc,char** argv){ if(argc>1&&std::string(argv[1])=="--window") return windowproof(GetModuleHandleW(nullptr)); return selftest(); }
