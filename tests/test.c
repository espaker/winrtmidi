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
    printf("  <- RX t=%.3fs %u bytes:", ts / 1e7, len);
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

int main(int argc, char **argv)
{
    HMODULE cb = LoadLibraryW(L"combase.dll");
    void *outs, *ins, *dis, *wrf;
    HS sel_out = NULL, sel_in = NULL, out_id, in_id;
    use_ro = argc > 1 && !strcmp(argv[1], "ro");
    WCS = (t_create)GetProcAddress(cb, "WindowsCreateString");
    WRAW = (t_raw)GetProcAddress(cb, "WindowsGetStringRawBuffer");
    pRoGetActivationFactory = (void *)GetProcAddress(cb, "RoGetActivationFactory");
    pRoInitialize = (void *)GetProcAddress(cb, "RoInitialize");
    pRoInitialize(1); /* MTA */
    if (!use_ro)
    {
        HMODULE m = LoadLibraryA(argc > 2 ? argv[2] : "winrtmidi.dll");
        if (!m) { printf("não carregou a DLL\n"); return 1; }
        pDllGetActivationFactory = (void *)GetProcAddress(m, "DllGetActivationFactory");
    }
    printf("modo: %s\n", use_ro ? "RoGetActivationFactory (registro)" : "DLL direta");

    outs = factory(L"Windows.Devices.Midi.MidiOutPort", &IID_IMidiOutPortStatics_);
    ins = factory(L"Windows.Devices.Midi.MidiInPort", &IID_IMidiInPortStatics_);
    dis = factory(L"Windows.Devices.Enumeration.DeviceInformation", &IID_IDeviceInformationStatics_);
    g_readerstatics = factory(L"Windows.Storage.Streams.DataReader", &IID_IDataReaderStatics_);
    wrf = factory(L"Windows.Storage.Streams.DataWriter", &IID_IActivationFactory_);
    if (!outs || !ins || !dis || !g_readerstatics || !wrf) return 1;

    CALL(outs, 7, HRESULT (*)(void *, HS *), &sel_out);
    CALL(ins, 7, HRESULT (*)(void *, HS *), &sel_in);
    {
        static WCHAR want[64] = L"Midi Through"; /* defina TEST_PORT pra escolher outra porta */
        GetEnvironmentVariableW(L"TEST_PORT", want, 64);
        out_id = find_port(dis, sel_out, want, "OUT");
        in_id = find_port(dis, sel_in, want, "IN");
    }
    if (!out_id || !in_id) { printf("Nanocore não encontrada\n"); return 2; }

    {
        void *op = NULL, *inport, *outport, *insp = NULL, *wr = NULL, *buf = NULL, *closable = NULL;
        INT64 token = 0;
        static BYTE ident[3000];
        int q;
        ident[0] = 0xf0; for (q = 1; q < 2999; q++) ident[q] = q & 0x7f; ident[2999] = 0xf7;
        HRESULT hr;

        CALL(ins, 6, HRESULT (*)(void *, HS, void **), in_id, &op);
        inport = await_op(op);
        CALL(outs, 6, HRESULT (*)(void *, HS, void **), out_id, &op);
        outport = await_op(op);
        printf("portas abertas: in=%p out=%p\n", inport, outport);
        if (!inport || !outport) return 3;
        hr = CALL(inport, 6, HRESULT (*)(void *, void *, INT64 *), &h_obj, &token);
        printf("MessageReceived += %08lx token=%lld\n", hr, (long long)token);

        CALL(wrf, 6, HRESULT (*)(void *, void **), &insp);
        CALL(insp, 0, HRESULT (*)(void *, const GUID *, void **), &IID_IDataWriter_, &wr);
        CALL(wr, 12, HRESULT (*)(void *, UINT32, const BYTE *), sizeof(ident), ident);
        CALL(wr, 31, HRESULT (*)(void *, void **), &buf);
        printf("-> TX sysex de %u bytes\n", (unsigned)sizeof(ident));
        hr = CALL(outport, 7, HRESULT (*)(void *, void *), buf);
        printf("SendBuffer -> %08lx\n", hr);

        Sleep(2500);
        printf("mensagens recebidas: %ld\n", g_count);
        CALL(inport, 7, HRESULT (*)(void *, INT64), token);
        CALL(inport, 0, HRESULT (*)(void *, const GUID *, void **), &IID_IClosable_, &closable);
        CALL(closable, 6, HRESULT (*)(void *));
        CALL(outport, 0, HRESULT (*)(void *, const GUID *, void **), &IID_IClosable_, &closable);
        CALL(closable, 6, HRESULT (*)(void *));
        printf("fechado\n");
    }
    return 0;
}
