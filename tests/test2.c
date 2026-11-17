/* teste: faz as mesmas chamadas WinRT que o midir (backend winrt).
 * uso: test.exe [ro]   (ro = ativa via RoGetActivationFactory/registro; senão carrega a DLL direto) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "guids.h"

typedef void *HS;
#define VT(p) (*(void ***)(p))
#define CALL(p, idx, T, ...) ((T)VT(p)[idx])(p, ##__VA_ARGS__)
typedef HRESULT (WINAPI *t_create)(const WCHAR *, UINT32, HS *);
typedef const WCHAR *(WINAPI *t_raw)(HS, UINT32 *);
static t_create WCS;
static t_raw WRAW;
static HRESULT (WINAPI *pRoGetActivationFactory)(HS, const GUID *, void **);
static HRESULT (WINAPI *pRoInitialize)(int);
static HRESULT (WINAPI *pDllGetActivationFactory)(HS, void **);

static HS hs(const WCHAR *s) { HS h; WCS(s, wcslen(s), &h); return h; }
static const WCHAR *raw(HS h) { const WCHAR *r = h ? WRAW(h, NULL) : NULL; return r ? r : L""; }

static int use_ro;
static void *factory(const WCHAR *cls, const GUID *iid)
{
    void *f = NULL, *s = NULL;
    HRESULT hr;
    if (use_ro) hr = pRoGetActivationFactory(hs(cls), iid, &s);
    else
    {
        hr = pDllGetActivationFactory(hs(cls), &f);
        if (SUCCEEDED(hr)) hr = CALL(f, 0, HRESULT (*)(void *, const GUID *, void **), iid, &s);
    }
    printf("factory %ls -> %08lx\n", cls, hr);
    return s;
}

static void *await_op(void *op)
{
    void *info = NULL, *res = NULL;
    int st = -1;
    HRESULT hr;
    CALL(op, 0, HRESULT (*)(void *, const GUID *, void **), &IID_IAsyncInfo_, &info);
    if (info) CALL(info, 7, HRESULT (*)(void *, int *), &st);
    hr = CALL(op, 8, HRESULT (*)(void *, void **), &res);
    if (FAILED(hr)) printf("  GetResults falhou %08lx (status %d)\n", hr, st);
    return res;
}

/* handler TypedEventHandler<MidiInPort, MidiMessageReceivedEventArgs> */
static void *g_readerstatics;
static LONG g_count;
static HRESULT STDMETHODCALLTYPE h_qi(void *t, const GUID *iid, void **o) { *o = t; return S_OK; }
static ULONG STDMETHODCALLTYPE h_ref(void *t) { return 2; }
static HRESULT STDMETHODCALLTYPE h_invoke(void *t, void *sender, void *args)
{
    void *msg = NULL, *buf = NULL, *rd = NULL;
    UINT32 len = 0;
    BYTE data[512];
    INT64 ts = 0;
    UINT32 k;
    CALL(args, 6, HRESULT (*)(void *, void **), &msg);
    CALL(msg, 6, HRESULT (*)(void *, INT64 *), &ts);
    CALL(msg, 7, HRESULT (*)(void *, void **), &buf);
    CALL(buf, 7, HRESULT (*)(void *, UINT32 *), &len);
    CALL(g_readerstatics, 6, HRESULT (*)(void *, void *, void **), buf, &rd);
    if (len > sizeof(data)) len = sizeof(data);
    CALL(rd, 14, HRESULT (*)(void *, UINT32, BYTE *), len, data);
    if(0)printf("  <- RX t=%.3fs %u bytes:", ts / 1e7, len);
    for (k = 0; k < len && k < 40; k++) printf(" %02x", data[k]);
    printf("\n");
    InterlockedIncrement(&g_count);
    return S_OK;
}
static void *h_vtbl[] = {h_qi, h_ref, h_ref, h_invoke};
static void *h_obj = h_vtbl;

static void *find_port(void *distatics, HS selector, const WCHAR *want, const char *what)
{
    void *op = NULL, *coll, *found = NULL;
    UINT32 n = 0, k;
    HRESULT hr = CALL(distatics, 10, HRESULT (*)(void *, HS, void **), selector, &op);
    printf("FindAllAsyncAqsFilter(%s) -> %08lx\n", what, hr);
    if (FAILED(hr)) return NULL;
    coll = await_op(op);
    if (!coll) return NULL;
    CALL(coll, 7, HRESULT (*)(void *, UINT32 *), &n);
    printf("  %u porta(s)\n", n);
    for (k = 0; k < n; k++)
    {
        void *di = NULL;
        HS id = NULL, name = NULL;
        CALL(coll, 6, HRESULT (*)(void *, UINT32, void **), k, &di);
        CALL(di, 6, HRESULT (*)(void *, HS *), &id);
        CALL(di, 7, HRESULT (*)(void *, HS *), &name);
        printf("  [%u] nome='%ls' id='%ls'\n", k, raw(name), raw(id));
        if (!found && wcsstr(raw(name), want)) found = id;
    }
    return found;
}


static void *g_ins, *g_outs, *g_dis, *g_wrf;
static HS g_sel_out, g_sel_in;
static int count_ports(HS sel)
{
    void *op = NULL, *coll; UINT32 n = 0;
    if (FAILED(CALL(g_dis, 10, HRESULT (*)(void *, HS, void **), sel, &op))) return -1;
    coll = await_op(op);
    if (!coll) return -1;
    CALL(coll, 7, HRESULT (*)(void *, UINT32 *), &n);
    return n;
}
static void *g_in, *g_out; static INT64 g_tok;
static int open_ports(void)
{
    void *op = NULL; HS oid, iid;
    oid = find_port(g_dis, g_sel_out, L"FakeNano", "OUT");
    iid = find_port(g_dis, g_sel_in, L"FakeNano", "IN");
    if (!oid || !iid) return 0;
    CALL(g_ins, 6, HRESULT (*)(void *, HS, void **), iid, &op); g_in = await_op(op);
    CALL(g_outs, 6, HRESULT (*)(void *, HS, void **), oid, &op); g_out = await_op(op);
    if (!g_in || !g_out) return 0;
    CALL(g_in, 6, HRESULT (*)(void *, void *, INT64 *), &h_obj, &g_tok);
    printf("  portas abertas\n");
    return 1;
}
static void close_ports(void)
{
    void *c = NULL;
    if (g_in) { CALL(g_in, 7, HRESULT (*)(void *, INT64), g_tok); CALL(g_in, 0, HRESULT (*)(void *, const GUID *, void **), &IID_IClosable_, &c); CALL(c, 6, HRESULT (*)(void *)); }
    if (g_out) { CALL(g_out, 0, HRESULT (*)(void *, const GUID *, void **), &IID_IClosable_, &c); CALL(c, 6, HRESULT (*)(void *)); }
    g_in = g_out = NULL;
    printf("  portas fechadas\n");
}
int main(int argc, char **argv)
{
    HMODULE cb = LoadLibraryW(L"combase.dll"), m;
    int reopen = argc > 1 && !strcmp(argv[1], "reopen"), t, last = -2;
    static BYTE sx[300];
    WCS = (t_create)GetProcAddress(cb, "WindowsCreateString");
    WRAW = (t_raw)GetProcAddress(cb, "WindowsGetStringRawBuffer");
    pRoInitialize = (void *)GetProcAddress(cb, "RoInitialize");
    pRoInitialize(1);
    m = LoadLibraryA("winrtmidi.dll");
    pDllGetActivationFactory = (void *)GetProcAddress(m, "DllGetActivationFactory");
    g_outs = factory(L"Windows.Devices.Midi.MidiOutPort", &IID_IMidiOutPortStatics_);
    g_ins = factory(L"Windows.Devices.Midi.MidiInPort", &IID_IMidiInPortStatics_);
    g_dis = factory(L"Windows.Devices.Enumeration.DeviceInformation", &IID_IDeviceInformationStatics_);
    g_readerstatics = factory(L"Windows.Storage.Streams.DataReader", &IID_IDataReaderStatics_);
    g_wrf = factory(L"Windows.Storage.Streams.DataWriter", &IID_IActivationFactory_);
    CALL(g_outs, 7, HRESULT (*)(void *, HS *), &g_sel_out);
    CALL(g_ins, 7, HRESULT (*)(void *, HS *), &g_sel_in);
    sx[0] = 0xf0; for (t = 1; t < 299; t++) sx[t] = t & 0x7f; sx[299] = 0xf7;
    printf("modo: %s\n", reopen ? "app fecha/reabre" : "portas sempre abertas");
    if (!open_ports()) { printf("FakeNano não encontrada no início\n"); return 2; }
    for (t = 0; t < 40; t++)
    {
        int n = find_port(g_dis, g_sel_out, L"FakeNano", "OUT") ? 1 : 0;
        LONG before = g_count;
        if (n != last) { printf("[%2ds] FindAll OUT: %d porta(s)\n", t, n); last = n; }
        if (reopen && n == 0 && g_out) close_ports();
        if (reopen && n > 0 && !g_out) open_ports();
        if (g_out)
        {
            void *insp = NULL, *wr = NULL, *buf = NULL; HRESULT hr;
            CALL(g_wrf, 6, HRESULT (*)(void *, void **), &insp);
            CALL(insp, 0, HRESULT (*)(void *, const GUID *, void **), &IID_IDataWriter_, &wr);
            CALL(wr, 12, HRESULT (*)(void *, UINT32, const BYTE *), sizeof(sx), sx);
            CALL(wr, 31, HRESULT (*)(void *, void **), &buf);
            hr = CALL(g_out, 7, HRESULT (*)(void *, void *), buf);
            Sleep(500);
            printf("[%2ds] envio %08lx, eco recebido: %s\n", t, hr, g_count > before ? "SIM" : "não");
            Sleep(500);
        }
        else Sleep(1000);
    }
    close_ports();
    return 0;
}
