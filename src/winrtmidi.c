/*
 * winrtmidi.dll — implementação mínima de Windows.Devices.Midi (WinRT MIDI 1.0) para Wine,
 * em cima do winmm. Cobre o que o crate midir (backend winrt) usa:
 *   Windows.Devices.Midi.MidiOutPort / MidiInPort (statics, portas, MessageReceived)
 *   Windows.Devices.Enumeration.DeviceInformation (proxy: MIDI aqui, resto vai pro Wine)
 *   Windows.Storage.Streams.DataWriter / DataReader / Buffer (mínimo)
 *
 * Log: variável WINRTMIDI_LOG (caminho Windows) ou C:\winrtmidi.log; WINRTMIDI_VERBOSE=1 loga cada mensagem.
 * Hotplug: lê /proc/asound/seq/clients (via Z:) e reabre os handles winmm quando a porta volta.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "guids.h"

#ifndef E_BOUNDS
#define E_BOUNDS ((HRESULT)0x8000000BL)
#endif
#ifndef RO_E_CLOSED
#define RO_E_CLOSED ((HRESULT)0x80000013L)
#endif
#ifndef E_ILLEGAL_METHOD_CALL
#define E_ILLEGAL_METHOD_CALL ((HRESULT)0x8000000EL)
#endif

typedef void *HSTRG;
#define VT(p) (*(void ***)(p))
#define F(x) ((void *)(x))

/* ------------------------------------------------------------------ util */

static HRESULT (WINAPI *pWindowsCreateString)(const WCHAR *, UINT32, HSTRG *);
static HRESULT (WINAPI *pWindowsDeleteString)(HSTRG);
static const WCHAR *(WINAPI *pWindowsGetStringRawBuffer)(HSTRG, UINT32 *);
static HRESULT (WINAPI *pWindowsDuplicateString)(HSTRG, HSTRG *);

static CRITICAL_SECTION log_cs;
static FILE *g_logf;
static int log_tried;

static void LOG(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&log_cs);
    if (!log_tried)
    {
        char path[MAX_PATH];
        log_tried = 1;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetEnvironmentVariableA("WINRTMIDI_LOG", path, sizeof(path))) strcpy(path, "C:\\winrtmidi.log");
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 4u << 20))
        {
            char old[MAX_PATH + 8];
            snprintf(old, sizeof(old), "%s.old", path);
            MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
        }
        g_logf = fopen(path, "a");
    }
    if (g_logf)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_logf, "%02u:%02u:%02u.%03u [%04lx] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                GetCurrentThreadId());
        va_start(ap, fmt);
        vfprintf(g_logf, fmt, ap);
        va_end(ap);
        fputc('\n', g_logf);
        fflush(g_logf);
    }
    LeaveCriticalSection(&log_cs);
}

static const char *dbgguid(const GUID *g)
{
    static char buf[8][40];
    static LONG idx;
    char *b = buf[InterlockedIncrement(&idx) & 7];
    sprintf(b, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2, g->Data3, g->Data4[0],
            g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return b;
}

static const char *dbgw(const WCHAR *w)
{
    static char buf[8][256];
    static LONG idx;
    char *b = buf[InterlockedIncrement(&idx) & 7];
    if (!w) return "(null)";
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(buf[0]), NULL, NULL);
    b[sizeof(buf[0]) - 1] = 0;
    return b;
}

static const char *dbghex(const BYTE *d, UINT32 n)
{
    static char buf[4][200];
    static LONG idx;
    char *b = buf[InterlockedIncrement(&idx) & 3], *p = b;
    UINT32 i;
    for (i = 0; i < n && i < 48; i++) p += sprintf(p, "%02x ", d[i]);
    if (n > 48) strcpy(p, "...");
    return b;
}

static HSTRG hs_new(const WCHAR *s)
{
    HSTRG h = NULL;
    if (s && *s) pWindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}

static const WCHAR *hs_raw(HSTRG h)
{
    const WCHAR *r = h ? pWindowsGetStringRawBuffer(h, NULL) : NULL;
    return r ? r : L"";
}

static HSTRG hs_dup(HSTRG h)
{
    HSTRG r = NULL;
    if (h) pWindowsDuplicateString(h, &r);
    return r;
}

static ULONG com_addref(void *p) { return ((ULONG (STDMETHODCALLTYPE *)(void *))VT(p)[1])(p); }
static ULONG com_release(void *p) { return ((ULONG (STDMETHODCALLTYPE *)(void *))VT(p)[2])(p); }
static HRESULT com_qi(void *p, const GUID *iid, void **out)
{
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, const GUID *, void **))VT(p)[0])(p, iid, out);
}

/* ------------------------------------------------------- objetos genéricos */

struct object;
typedef struct iface { void *const *vtbl; struct object *outer; } iface;
typedef struct ifentry { const GUID *iid; int slot; } ifentry;
typedef struct objtype
{
    const char *dbg;
    const WCHAR *rcname;
    void (*destroy)(struct object *);
    void (*close)(struct object *);
    HRESULT (*qi_fallback)(struct object *, const GUID *, void **);
    ifentry entries[4];
} objtype;

#define MAXIF 3
typedef struct object
{
    iface ifs[MAXIF];
    LONG ref;
    const objtype *type;
    const GUID *dyn_iid; /* IID parametrizado extra -> slot 0 */
    IUnknown *ftm;
} object;

#define OUTER(T, i) ((T *)(((iface *)(i))->outer))

static void obj_init(object *o, const objtype *t, void *const *v0, void *const *v1, void *const *v2)
{
    memset(o, 0, sizeof(*o));
    o->ref = 1;
    o->type = t;
    o->ifs[0].vtbl = v0; o->ifs[0].outer = o;
    o->ifs[1].vtbl = v1; o->ifs[1].outer = o;
    o->ifs[2].vtbl = v2; o->ifs[2].outer = o;
}

static ULONG STDMETHODCALLTYPE obj_AddRef(iface *i) { return InterlockedIncrement(&i->outer->ref); }

static void obj_free_base(object *o)
{
    if (o->ftm) o->ftm->lpVtbl->Release(o->ftm);
}

static ULONG STDMETHODCALLTYPE obj_Release(iface *i)
{
    object *o = i->outer;
    LONG r = InterlockedDecrement(&o->ref);
    if (!r && o->type->destroy) o->type->destroy(o);
    return r;
}

static HRESULT STDMETHODCALLTYPE obj_QI(iface *i, const GUID *riid, void **out)
{
    object *o = i->outer;
    const ifentry *e;
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(riid, &IID_IUnknown_) || IsEqualGUID(riid, &IID_IInspectable_) ||
        IsEqualGUID(riid, &IID_IAgileObject_) || (o->dyn_iid && IsEqualGUID(riid, o->dyn_iid)))
    {
        *out = &o->ifs[0];
        obj_AddRef(i);
        return S_OK;
    }
    for (e = o->type->entries; e->iid; e++)
    {
        if (IsEqualGUID(riid, e->iid))
        {
            *out = &o->ifs[e->slot];
            obj_AddRef(i);
            return S_OK;
        }
    }
    if (IsEqualGUID(riid, &IID_IMarshal_))
    {
        if (!o->ftm && FAILED(CoCreateFreeThreadedMarshaler((IUnknown *)&o->ifs[0], &o->ftm))) return E_NOINTERFACE;
        return o->ftm->lpVtbl->QueryInterface(o->ftm, riid, out);
    }
    if (o->type->qi_fallback) return o->type->qi_fallback(o, riid, out);
    LOG("%s: QI desconhecida %s", o->type->dbg, dbgguid(riid));
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE obj_GetIids(iface *i, ULONG *count, GUID **iids)
{
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE obj_GetRuntimeClassName(iface *i, HSTRG *name)
{
    *name = hs_new(i->outer->type->rcname);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE obj_GetTrustLevel(iface *i, int *lvl)
{
    *lvl = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE notimpl(void) { return E_NOTIMPL; }

#define UNK F(obj_QI), F(obj_AddRef), F(obj_Release)
#define INSP UNK, F(obj_GetIids), F(obj_GetRuntimeClassName), F(obj_GetTrustLevel)

static HRESULT STDMETHODCALLTYPE closable_Close(iface *i)
{
    object *o = i->outer;
    if (o->type->close) o->type->close(o);
    return S_OK;
}
static void *const vt_closable[] = {INSP, F(closable_Close)};

/* --------------------------------------------------------------- Buffer */

struct buffer { object o; BYTE *data; UINT32 cap, len; };

static void buffer_destroy(object *o)
{
    struct buffer *b = (struct buffer *)o;
    obj_free_base(o);
    free(b->data);
    free(b);
}

static HRESULT STDMETHODCALLTYPE buf_get_Capacity(iface *i, UINT32 *v) { *v = OUTER(struct buffer, i)->cap; return S_OK; }
static HRESULT STDMETHODCALLTYPE buf_get_Length(iface *i, UINT32 *v) { *v = OUTER(struct buffer, i)->len; return S_OK; }
static HRESULT STDMETHODCALLTYPE buf_put_Length(iface *i, UINT32 v)
{
    struct buffer *b = OUTER(struct buffer, i);
    if (v > b->cap) return E_INVALIDARG;
    b->len = v;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE buf_Buffer(iface *i, BYTE **p) { *p = OUTER(struct buffer, i)->data; return S_OK; }

static void *const vt_buffer[] = {INSP, F(buf_get_Capacity), F(buf_get_Length), F(buf_put_Length)};
static void *const vt_bufaccess[] = {UNK, F(buf_Buffer)};
static const objtype type_buffer = {"Buffer", L"Windows.Storage.Streams.Buffer", buffer_destroy, NULL, NULL,
                                    {{&IID_IBuffer_, 0}, {&IID_IBufferByteAccess_, 1}}};

static struct buffer *buffer_create(const BYTE *src, UINT32 len)
{
    struct buffer *b = calloc(1, sizeof(*b));
    obj_init(&b->o, &type_buffer, vt_buffer, vt_bufaccess, NULL);
    b->cap = b->len = len;
    b->data = malloc(len ? len : 1);
    if (src && len) memcpy(b->data, src, len);
    return b;
}

/* extrai bytes de qualquer IBuffer (nosso ou de outro) */
static HRESULT ibuffer_bytes(void *ibuf, BYTE **data, UINT32 *len)
{
    void *ba = NULL;
    HRESULT hr;
    if (!ibuf) return E_POINTER;
    if (VT(ibuf) == (void **)vt_buffer)
    {
        struct buffer *b = OUTER(struct buffer, ibuf);
        *data = b->data;
        *len = b->len;
        return S_OK;
    }
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32 *))VT(ibuf)[7])(ibuf, len);
    if (FAILED(hr)) return hr;
    hr = com_qi(ibuf, &IID_IBufferByteAccess_, &ba);
    if (FAILED(hr)) { LOG("IBuffer estrangeiro sem IBufferByteAccess"); return hr; }
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, BYTE **))VT(ba)[3])(ba, data);
    com_release(ba);
    return hr;
}

/* --------------------------------------------------------- IAsyncOperation */

struct asyncop { object o; void *result; HRESULT hr; void *handler; };

static void asyncop_destroy(object *o)
{
    struct asyncop *a = (struct asyncop *)o;
    if (a->result) com_release(a->result);
    if (a->handler) com_release(a->handler);
    obj_free_base(o);
    free(a);
}

static HRESULT STDMETHODCALLTYPE op_put_Completed(iface *i, void *handler)
{
    struct asyncop *a = OUTER(struct asyncop, i);
    if (a->handler) return E_ILLEGAL_METHOD_CALL;
    if (!handler) return S_OK;
    com_addref(handler);
    a->handler = handler;
    /* já está completa: invoca imediatamente (Invoke(asyncInfo, status)) */
    ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, int))VT(handler)[3])(handler, &a->o.ifs[0],
                                                                         SUCCEEDED(a->hr) ? 1 : 3);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE op_get_Completed(iface *i, void **handler)
{
    struct asyncop *a = OUTER(struct asyncop, i);
    *handler = a->handler;
    if (a->handler) com_addref(a->handler);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE op_GetResults(iface *i, void **res)
{
    struct asyncop *a = OUTER(struct asyncop, i);
    *res = NULL;
    if (FAILED(a->hr)) return a->hr;
    *res = a->result;
    if (a->result) com_addref(a->result);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE info_get_Id(iface *i, UINT32 *id) { *id = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE info_get_Status(iface *i, int *st)
{
    *st = SUCCEEDED(OUTER(struct asyncop, i)->hr) ? 1 : 3;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE info_get_ErrorCode(iface *i, HRESULT *hr)
{
    *hr = OUTER(struct asyncop, i)->hr;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE info_Cancel(iface *i) { return S_OK; }
static HRESULT STDMETHODCALLTYPE info_Close(iface *i) { return S_OK; }

static void *const vt_asyncop[] = {INSP, F(op_put_Completed), F(op_get_Completed), F(op_GetResults)};
static void *const vt_asyncinfo[] = {INSP, F(info_get_Id), F(info_get_Status), F(info_get_ErrorCode),
                                     F(info_Cancel), F(info_Close)};
static const objtype type_asyncop = {"AsyncOperation", L"Windows.Foundation.IAsyncOperation`1", asyncop_destroy,
                                     NULL, NULL, {{&IID_IAsyncInfo_, 1}}};

/* result: ponteiro já com referência (é "consumido"); pode ser NULL com hr de erro */
static void *asyncop_create(const GUID *opiid, void *result, HRESULT hr)
{
    struct asyncop *a = calloc(1, sizeof(*a));
    obj_init(&a->o, &type_asyncop, vt_asyncop, vt_asyncinfo, NULL);
    a->o.dyn_iid = opiid;
    a->result = result;
    a->hr = hr;
    return &a->o.ifs[0];
}

/* ------------------------------------------------------------ portas winmm */

struct portinfo { BOOL in; UINT dev; WCHAR raw[MAXPNAMELEN + 1]; WCHAR name[MAXPNAMELEN + 1]; WCHAR id[160]; };

#define ID_PREFIX L"\\\\?\\WINEMIDI#"

static BOOL port_filtered(const WCHAR *raw)
{
    static int all = -1;
    if (all < 0)
    {
        char v[8];
        all = GetEnvironmentVariableA("WINRTMIDI_ALLPORTS", v, sizeof(v)) && v[0] == '1';
    }
    if (all) return FALSE;
    return !wcsncmp(raw, L"PipeWire-", 9) || !wcsncmp(raw, L"Midi Through", 12) || !wcsncmp(raw, L"ncbridge", 8);
}

/* "Nanocore - Nanocore MIDI 1" -> "Nanocore" (como o Windows nomeia porta USB-MIDI única) */
static void friendly_name(const WCHAR *raw, WCHAR *out)
{
    const WCHAR *sep = wcsstr(raw, L" - ");
    char v[8];
    wcscpy(out, raw);
    if (GetEnvironmentVariableA("WINRTMIDI_RAWNAMES", v, sizeof(v)) && v[0] == '1') return;
    if (sep)
    {
        size_t cl = sep - raw;
        if (!wcsncmp(sep + 3, raw, cl))
        {
            wcsncpy(out, raw, cl);
            out[cl] = 0;
        }
    }
}

/* ------------------------------------------------- hotplug (ALSA seq) ---
 * O winmm do Wine enumera as portas uma vez só e não percebe a pedaleira saindo/voltando.
 * Lemos /proc/asound/seq/clients (via Z:) pra saber quais portas existem de verdade agora.
 * Cada nome tem uma "época": muda toda vez que a porta reaparece => portas abertas reabrem
 * o handle winmm (o que refaz a assinatura ALSA do Wine). */

#define MAXALSA 64
struct alsaport { WCHAR combo[MAXPNAMELEN]; WCHAR port[MAXPNAMELEN]; int client; LONG epoch; };
static CRITICAL_SECTION pres_cs;
static struct alsaport pres[MAXALSA];
static int pres_n, pres_state = -1; /* -1 nunca leu, 0 ilegível (assume tudo presente), 1 ok */
static DWORD pres_tick;
static LONG pres_epoch;

static int verbose(void)
{
    static int v = -1;
    if (v < 0)
    {
        char s[8];
        v = GetEnvironmentVariableA("WINRTMIDI_VERBOSE", s, sizeof(s)) && s[0] == '1';
    }
    return v;
}

static void wtrunc(WCHAR *dst, const char *utf8)
{
    WCHAR tmp[256];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, tmp, 256)) tmp[0] = 0;
    tmp[255] = 0;
    wcsncpy(dst, tmp, MAXPNAMELEN - 1);
    dst[MAXPNAMELEN - 1] = 0;
}

static int read_seq_clients(char *buf, int cap)
{
    static const WCHAR *paths[] = {L"Z:\\proc\\asound\\seq\\clients", L"\\\\?\\unix\\proc\\asound\\seq\\clients"};
    int k;
    for (k = 0; k < 2; k++)
    {
        HANDLE f = CreateFileW(paths[k], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        DWORD got;
        int n = 0;
        if (f == INVALID_HANDLE_VALUE) continue;
        while (n < cap - 1 && ReadFile(f, buf + n, cap - 1 - n, &got, NULL) && got) n += got;
        CloseHandle(f);
        buf[n] = 0;
        if (n) return n;
    }
    return -1;
}

static void pres_refresh(void)
{
    static char buf[32768];
    struct alsaport np[MAXALSA];
    char cname[128] = "", *line, *next;
    int nn = 0, client = -1, i, j, first;
    DWORD now = GetTickCount();

    EnterCriticalSection(&pres_cs);
    if (pres_state >= 0 && now - pres_tick < 300) { LeaveCriticalSection(&pres_cs); return; }
    pres_tick = now;
    if (read_seq_clients(buf, sizeof(buf)) < 0)
    {
        if (pres_state != 0) LOG("hotplug: não consegui ler /proc/asound/seq/clients; hotplug desligado");
        pres_state = 0;
        LeaveCriticalSection(&pres_cs);
        return;
    }
    for (line = buf; line && *line; line = next)
    {
        char *q1, *q2;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        q1 = strchr(line, '"');
        q2 = q1 ? strrchr(q1 + 1, '"') : NULL;
        if (!q1 || !q2) continue;
        *q2 = 0;
        if (!strncmp(line, "Client", 6))
        {
            client = atoi(line + 6);
            snprintf(cname, sizeof(cname), "%s", q1 + 1);
        }
        else if (!strncmp(line, "  Port", 6) && client >= 0 && nn < MAXALSA)
        {
            char combo[300];
            /* mesma regra do winealsa: "cliente - porta" se couber, senão só a porta */
            if (strlen(cname) + strlen(q1 + 1) + 3 < MAXPNAMELEN) snprintf(combo, sizeof(combo), "%s - %s", cname, q1 + 1);
            else snprintf(combo, sizeof(combo), "%s", q1 + 1);
            wtrunc(np[nn].combo, combo);
            wtrunc(np[nn].port, q1 + 1);
            np[nn].client = client;
            np[nn].epoch = 0;
            nn++;
        }
    }
    first = pres_state != 1;
    for (i = 0; i < nn; i++)
    {
        for (j = 0; j < pres_n; j++)
            if (!wcscmp(pres[j].combo, np[i].combo)) break;
        if (j < pres_n)
        {
            np[i].epoch = pres[j].epoch;
            if (pres[j].client != np[i].client)
                LOG("hotplug: AVISO %s mudou de cliente ALSA %d -> %d (o Wine guardou o antigo; fixe o índice da placa ou reabra o app)",
                    dbgw(np[i].combo), pres[j].client, np[i].client);
        }
        else
        {
            np[i].epoch = ++pres_epoch;
            if (!first && !port_filtered(np[i].combo) && wcsncmp(np[i].combo, L"WINE ", 5)) LOG("hotplug: porta apareceu: %s (cliente %d)", dbgw(np[i].combo), np[i].client);
        }
    }
    for (j = 0; j < pres_n && !first; j++)
    {
        for (i = 0; i < nn; i++)
            if (!wcscmp(pres[j].combo, np[i].combo)) break;
        if (i == nn && !port_filtered(pres[j].combo) && wcsncmp(pres[j].combo, L"WINE ", 5)) LOG("hotplug: porta sumiu: %s", dbgw(pres[j].combo));
    }
    memcpy(pres, np, nn * sizeof(np[0]));
    pres_n = nn;
    pres_state = 1;
    LeaveCriticalSection(&pres_cs);
}

/* época atual da porta winmm com esse nome; 0 = ausente; 1 se não dá pra saber */
static LONG port_epoch(const WCHAR *raw)
{
    LONG e = 0;
    int j;
    pres_refresh();
    EnterCriticalSection(&pres_cs);
    if (pres_state != 1) e = 1;
    else
    {
        for (j = 0; j < pres_n && !e; j++)
            if (!wcscmp(pres[j].combo, raw)) e = pres[j].epoch;
        for (j = 0; j < pres_n && !e; j++)
            if (!wcscmp(pres[j].port, raw)) e = pres[j].epoch;
    }
    LeaveCriticalSection(&pres_cs);
    return e;
}

static void make_portinfo(BOOL in, UINT dev, const WCHAR *raw, struct portinfo *p)
{
    p->in = in;
    p->dev = dev;
    wcsncpy(p->raw, raw, MAXPNAMELEN);
    p->raw[MAXPNAMELEN] = 0;
    friendly_name(p->raw, p->name);
    swprintf(p->id, 160, ID_PREFIX L"%ls#%u#%ls", in ? L"IN" : L"OUT", dev, p->raw);
}

static int enum_ports(BOOL in, struct portinfo *out, int max)
{
    UINT n = in ? midiInGetNumDevs() : midiOutGetNumDevs(), i;
    int c = 0;
    for (i = 0; i < n && c < max; i++)
    {
        WCHAR raw[MAXPNAMELEN + 1];
        if (in)
        {
            MIDIINCAPSW caps;
            if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
            wcscpy(raw, caps.szPname);
        }
        else
        {
            MIDIOUTCAPSW caps;
            if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
            if (caps.wTechnology != MOD_MIDIPORT) continue; /* ignora sintetizadores */
            wcscpy(raw, caps.szPname);
        }
        if (port_filtered(raw)) continue;
        if (!port_epoch(raw)) continue; /* desplugada */
        make_portinfo(in, i, raw, &out[c++]);
    }
    return c;
}

/* resolve um id nosso -> portinfo atual (por índice, conferindo o nome; senão procura pelo nome) */
static BOOL find_port(const WCHAR *id, struct portinfo *p)
{
    struct portinfo list[64];
    const WCHAR *s;
    BOOL in;
    unsigned dev;
    int n, i;
    size_t pl = wcslen(ID_PREFIX);
    if (wcsncmp(id, ID_PREFIX, pl)) return FALSE;
    s = id + pl;
    if (!wcsncmp(s, L"IN#", 3)) { in = TRUE; s += 3; }
    else if (!wcsncmp(s, L"OUT#", 4)) { in = FALSE; s += 4; }
    else return FALSE;
    dev = wcstoul(s, (WCHAR **)&s, 10);
    if (*s == L'#') s++;
    n = enum_ports(in, list, 64);
    for (i = 0; i < n; i++)
        if (list[i].dev == dev && !wcscmp(list[i].raw, s)) { *p = list[i]; return TRUE; }
    for (i = 0; i < n; i++)
        if (!wcscmp(list[i].raw, s)) { *p = list[i]; return TRUE; }
    return FALSE;
}

/* ----------------------------------------------------- DeviceInformation */

struct devinfo { object o; HSTRG id, name; };

static void devinfo_destroy(object *o)
{
    struct devinfo *d = (struct devinfo *)o;
    pWindowsDeleteString(d->id);
    pWindowsDeleteString(d->name);
    obj_free_base(o);
    free(d);
}
static HRESULT STDMETHODCALLTYPE di_get_Id(iface *i, HSTRG *v) { *v = hs_dup(OUTER(struct devinfo, i)->id); return S_OK; }
static HRESULT STDMETHODCALLTYPE di_get_Name(iface *i, HSTRG *v) { *v = hs_dup(OUTER(struct devinfo, i)->name); return S_OK; }
static HRESULT STDMETHODCALLTYPE di_get_IsEnabled(iface *i, BYTE *v) { *v = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE di_get_IsDefault(iface *i, BYTE *v) { *v = 0; return S_OK; }

static void *const vt_devinfo[] = {INSP, F(di_get_Id), F(di_get_Name), F(di_get_IsEnabled), F(di_get_IsDefault),
                                   F(notimpl), F(notimpl), F(notimpl), F(notimpl), F(notimpl)};
static const objtype type_devinfo = {"DeviceInformation", L"Windows.Devices.Enumeration.DeviceInformation",
                                     devinfo_destroy, NULL, NULL, {{&IID_IDeviceInformation_, 0}}};

static struct devinfo *devinfo_create(const struct portinfo *p)
{
    struct devinfo *d = calloc(1, sizeof(*d));
    obj_init(&d->o, &type_devinfo, vt_devinfo, NULL, NULL);
    d->id = hs_new(p->id);
    d->name = hs_new(p->name);
    return d;
}

/* coleção (IVectorView<DeviceInformation> + IIterable) */
struct devcoll { object o; UINT32 n; struct devinfo **items; };
struct deviter { object o; struct devcoll *c; UINT32 pos; };

static void devcoll_destroy(object *o)
{
    struct devcoll *c = (struct devcoll *)o;
    UINT32 k;
    for (k = 0; k < c->n; k++) obj_Release(&c->items[k]->o.ifs[0]);
    free(c->items);
    obj_free_base(o);
    free(c);
}
static HRESULT STDMETHODCALLTYPE vv_GetAt(iface *i, UINT32 idx, void **v)
{
    struct devcoll *c = OUTER(struct devcoll, i);
    *v = NULL;
    if (idx >= c->n) return E_BOUNDS;
    *v = &c->items[idx]->o.ifs[0];
    obj_AddRef(*v);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE vv_get_Size(iface *i, UINT32 *v) { *v = OUTER(struct devcoll, i)->n; return S_OK; }
static HRESULT STDMETHODCALLTYPE vv_IndexOf(iface *i, void *item, UINT32 *idx, BYTE *found)
{
    struct devcoll *c = OUTER(struct devcoll, i);
    UINT32 k;
    *found = 0;
    *idx = 0;
    for (k = 0; k < c->n; k++)
        if (item == &c->items[k]->o.ifs[0]) { *idx = k; *found = 1; break; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE vv_GetMany(iface *i, UINT32 start, UINT32 cap, void **items, UINT32 *actual)
{
    struct devcoll *c = OUTER(struct devcoll, i);
    UINT32 k = 0;
    while (start + k < c->n && k < cap)
    {
        items[k] = &c->items[start + k]->o.ifs[0];
        obj_AddRef(items[k]);
        k++;
    }
    *actual = k;
    return S_OK;
}

static void deviter_destroy(object *o)
{
    struct deviter *it = (struct deviter *)o;
    obj_Release(&it->c->o.ifs[0]);
    obj_free_base(o);
    free(it);
}
static HRESULT STDMETHODCALLTYPE itr_get_Current(iface *i, void **v)
{
    struct deviter *it = OUTER(struct deviter, i);
    return vv_GetAt(&it->c->o.ifs[0], it->pos, v);
}
static HRESULT STDMETHODCALLTYPE itr_get_HasCurrent(iface *i, BYTE *v)
{
    struct deviter *it = OUTER(struct deviter, i);
    *v = it->pos < it->c->n;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE itr_MoveNext(iface *i, BYTE *v)
{
    struct deviter *it = OUTER(struct deviter, i);
    if (it->pos < it->c->n) it->pos++;
    *v = it->pos < it->c->n;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE itr_GetMany(iface *i, UINT32 cap, void **items, UINT32 *actual)
{
    struct deviter *it = OUTER(struct deviter, i);
    HRESULT hr = vv_GetMany(&it->c->o.ifs[0], it->pos, cap, items, actual);
    it->pos += *actual;
    return hr;
}
static void *const vt_iterator[] = {INSP, F(itr_get_Current), F(itr_get_HasCurrent), F(itr_MoveNext), F(itr_GetMany)};
static const objtype type_deviter = {"DeviceIterator", L"Windows.Foundation.Collections.IIterator`1", deviter_destroy,
                                     NULL, NULL, {{&IID_IIterator_DeviceInformation, 0}}};

static HRESULT STDMETHODCALLTYPE it_First(iface *i, void **out)
{
    struct devcoll *c = OUTER(struct devcoll, i);
    struct deviter *it = calloc(1, sizeof(*it));
    obj_init(&it->o, &type_deviter, vt_iterator, NULL, NULL);
    it->c = c;
    obj_AddRef(&c->o.ifs[0]);
    *out = &it->o.ifs[0];
    return S_OK;
}

static void *const vt_vecview[] = {INSP, F(vv_GetAt), F(vv_get_Size), F(vv_IndexOf), F(vv_GetMany)};
static void *const vt_iterable[] = {INSP, F(it_First)};
static const objtype type_devcoll = {"DeviceInformationCollection",
                                     L"Windows.Devices.Enumeration.DeviceInformationCollection", devcoll_destroy, NULL,
                                     NULL,
                                     {{&IID_IVectorView_DeviceInformation, 0}, {&IID_IIterable_DeviceInformation, 1}}};

static struct devcoll *devcoll_create(BOOL in)
{
    struct portinfo list[64];
    struct devcoll *c = calloc(1, sizeof(*c));
    int n = enum_ports(in, list, 64), k;
    obj_init(&c->o, &type_devcoll, vt_vecview, vt_iterable, NULL);
    c->items = calloc(n ? n : 1, sizeof(*c->items));
    for (k = 0; k < n; k++) c->items[k] = devinfo_create(&list[k]);
    c->n = n;
    return c;
}

/* ----------------------------------------------------------- MidiOutPort */

struct mop
{
    object o; HMIDIOUT h; HSTRG id; CRITICAL_SECTION cs; BYTE running; BOOL closed;
    UINT dev; WCHAR raw[MAXPNAMELEN + 1]; LONG epoch; BOOL hopen, gone; DWORD dropped;
};

/* com m->cs. TRUE se dá pra enviar; reabre o handle se a porta voltou */
static BOOL mop_check(struct mop *m)
{
    LONG e = port_epoch(m->raw);
    MMRESULT r;
    if (!e)
    {
        if (!m->gone) LOG("MidiOutPort: %s desconectada, descartando envios até voltar", dbgw(m->raw));
        m->gone = TRUE;
        return FALSE;
    }
    if (e == m->epoch && m->hopen) return TRUE;
    if (m->hopen) { midiOutReset(m->h); midiOutClose(m->h); m->hopen = FALSE; }
    r = midiOutOpen(&m->h, m->dev, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR)
    {
        LOG("MidiOutPort: reabrir %s falhou (%u), tento de novo no próximo envio", dbgw(m->raw), r);
        return FALSE;
    }
    m->hopen = TRUE;
    m->epoch = e;
    m->gone = FALSE;
    m->running = 0;
    LOG("MidiOutPort: %s reconectada (%lu envio(s) descartado(s) enquanto fora)", dbgw(m->raw), m->dropped);
    m->dropped = 0;
    return TRUE;
}

static void mop_close(object *o)
{
    struct mop *m = (struct mop *)o;
    EnterCriticalSection(&m->cs);
    if (!m->closed)
    {
        m->closed = TRUE;
        if (m->hopen)
        {
            midiOutReset(m->h);
            midiOutClose(m->h);
            m->hopen = FALSE;
        }
        LOG("MidiOutPort fechada %s", dbgw(hs_raw(m->id)));
    }
    LeaveCriticalSection(&m->cs);
}

static void mop_destroy(object *o)
{
    struct mop *m = (struct mop *)o;
    mop_close(o);
    DeleteCriticalSection(&m->cs);
    pWindowsDeleteString(m->id);
    obj_free_base(o);
    free(m);
}

static int short_len(BYTE st)
{
    if (st < 0xf0) return ((st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0) ? 2 : 3;
    switch (st)
    {
    case 0xf1: case 0xf3: return 2;
    case 0xf2: return 3;
    default: return 1;
    }
}

static HRESULT mop_send_bytes(struct mop *m, const BYTE *d, UINT32 n)
{
    UINT32 p = 0;
    HRESULT hr = S_OK;
    EnterCriticalSection(&m->cs);
    if (m->closed) { LeaveCriticalSection(&m->cs); return RO_E_CLOSED; }
    /* desplugada: finge que enviou (o app percebe a saída pelo FindAll; erro aqui pode derrubá-lo) */
    if (!mop_check(m)) { m->dropped++; LeaveCriticalSection(&m->cs); return S_OK; }
    while (p < n)
    {
        if (d[p] == 0xf0)
        {
            UINT32 e = p + 1;
            MIDIHDR hdr;
            DWORD t0;
            MMRESULT r;
            while (e < n && d[e] != 0xf7) e++;
            if (e < n) e++; /* inclui F7 */
            memset(&hdr, 0, sizeof(hdr));
            hdr.dwBufferLength = hdr.dwBytesRecorded = e - p;
            hdr.lpData = malloc(hdr.dwBufferLength);
            memcpy(hdr.lpData, d + p, hdr.dwBufferLength);
            midiOutPrepareHeader(m->h, &hdr, sizeof(hdr));
            r = midiOutLongMsg(m->h, &hdr, sizeof(hdr));
            if (r != MMSYSERR_NOERROR) { LOG("midiOutLongMsg falhou: %u", r); hr = E_FAIL; }
            else
            {
                t0 = GetTickCount();
                while (!(hdr.dwFlags & MHDR_DONE) && GetTickCount() - t0 < 5000) Sleep(1);
            }
            midiOutUnprepareHeader(m->h, &hdr, sizeof(hdr));
            free(hdr.lpData);
            p = e;
            m->running = 0;
        }
        else
        {
            BYTE st = d[p];
            int len, k;
            DWORD msg;
            if (st < 0x80)
            {
                if (!m->running) { p++; continue; } /* byte de dados solto */
                st = m->running;
                len = short_len(st) - 1;
                msg = st;
                for (k = 0; k < len && p + k < n; k++) msg |= (DWORD)d[p + k] << (8 * (k + 1));
                p += len;
            }
            else
            {
                len = short_len(st);
                msg = 0;
                for (k = 0; k < len && p + k < n; k++) msg |= (DWORD)d[p + k] << (8 * k);
                p += len;
                if (st < 0xf0) m->running = st;
                else if (st < 0xf8) m->running = 0;
            }
            if (midiOutShortMsg(m->h, msg) != MMSYSERR_NOERROR) hr = E_FAIL;
        }
    }
    LeaveCriticalSection(&m->cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE mop_SendBuffer(iface *i, void *ibuf)
{
    struct mop *m = OUTER(struct mop, i);
    BYTE *d;
    UINT32 n;
    HRESULT hr = ibuffer_bytes(ibuf, &d, &n);
    if (FAILED(hr)) return hr;
    if (verbose()) LOG("OUT %u bytes: %s", n, dbghex(d, n));
    return mop_send_bytes(m, d, n);
}

static HRESULT STDMETHODCALLTYPE mop_SendMessage(iface *i, void *msg)
{
    void *buf = NULL;
    HRESULT hr;
    if (!msg) return E_POINTER;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))VT(msg)[7])(msg, &buf); /* IMidiMessage::RawData */
    if (FAILED(hr)) return hr;
    hr = mop_SendBuffer(i, buf);
    com_release(buf);
    return hr;
}

static HRESULT STDMETHODCALLTYPE mop_get_DeviceId(iface *i, HSTRG *v) { *v = hs_dup(OUTER(struct mop, i)->id); return S_OK; }

static void *const vt_mop[] = {INSP, F(mop_SendMessage), F(mop_SendBuffer), F(mop_get_DeviceId)};
static const objtype type_mop = {"MidiOutPort", L"Windows.Devices.Midi.MidiOutPort", mop_destroy, mop_close, NULL,
                                 {{&IID_IMidiOutPort_, 0}, {&IID_IClosable_, 1}}};

static HRESULT mop_open(HSTRG id, struct mop **out)
{
    struct portinfo p;
    struct mop *m;
    MMRESULT r;
    HMIDIOUT h;
    if (!find_port(hs_raw(id), &p) || p.in) { LOG("MidiOutPort.FromIdAsync: id desconhecido %s", dbgw(hs_raw(id))); return E_INVALIDARG; }
    r = midiOutOpen(&h, p.dev, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) { LOG("midiOutOpen(%u) falhou: %u", p.dev, r); return E_ACCESSDENIED; }
    m = calloc(1, sizeof(*m));
    obj_init(&m->o, &type_mop, vt_mop, vt_closable, NULL);
    InitializeCriticalSection(&m->cs);
    m->h = h;
    m->hopen = TRUE;
    m->dev = p.dev;
    wcscpy(m->raw, p.raw);
    m->epoch = port_epoch(p.raw);
    m->id = hs_dup(id);
    LOG("MidiOutPort aberta: %s (winmm %u)", dbgw(p.raw), p.dev);
    *out = m;
    return S_OK;
}

/* ------------------------------------------------- MidiMessage + EventArgs */

struct mmsg { object o; struct buffer *buf; INT64 ts; int type; };
struct margs { object o; struct mmsg *m; };

static void mmsg_destroy(object *o)
{
    struct mmsg *m = (struct mmsg *)o;
    obj_Release(&m->buf->o.ifs[0]);
    obj_free_base(o);
    free(m);
}
static HRESULT STDMETHODCALLTYPE mm_get_Timestamp(iface *i, INT64 *v) { *v = OUTER(struct mmsg, i)->ts; return S_OK; }
static HRESULT STDMETHODCALLTYPE mm_get_RawData(iface *i, void **v)
{
    struct mmsg *m = OUTER(struct mmsg, i);
    *v = &m->buf->o.ifs[0];
    obj_AddRef(*v);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE mm_get_Type(iface *i, int *v) { *v = OUTER(struct mmsg, i)->type; return S_OK; }
static void *const vt_mmsg[] = {INSP, F(mm_get_Timestamp), F(mm_get_RawData), F(mm_get_Type)};
static const objtype type_mmsg = {"MidiMessage", L"Windows.Devices.Midi.MidiSystemExclusiveMessage", mmsg_destroy,
                                  NULL, NULL, {{&IID_IMidiMessage_, 0}}};

static void margs_destroy(object *o)
{
    struct margs *a = (struct margs *)o;
    obj_Release(&a->m->o.ifs[0]);
    obj_free_base(o);
    free(a);
}
static HRESULT STDMETHODCALLTYPE ma_get_Message(iface *i, void **v)
{
    struct margs *a = OUTER(struct margs, i);
    *v = &a->m->o.ifs[0];
    obj_AddRef(*v);
    return S_OK;
}
static void *const vt_margs[] = {INSP, F(ma_get_Message)};
static const objtype type_margs = {"MidiMessageReceivedEventArgs", L"Windows.Devices.Midi.MidiMessageReceivedEventArgs",
                                   margs_destroy, NULL, NULL, {{&IID_IMidiMessageReceivedEventArgs_, 0}}};

/* ------------------------------------------------------------ MidiInPort */

#define NBUF 16
#define BUFSZ 65536
#define QLEN 4096
#define MAXH 16

struct qitem { int kind; DWORD p1; MIDIHDR *hdr; HMIDIIN h; };
struct mip
{
    object o;
    HMIDIIN h;
    HSTRG id;
    CRITICAL_SECTION cs;
    struct { INT64 token; void *h; } handlers[MAXH];
    int nh;
    INT64 next_token;
    MIDIHDR hdrs[NBUF];
    CRITICAL_SECTION qcs;
    HANDLE qev, thread;
    DWORD tid;
    struct qitem q[QLEN];
    int qh, qt;
    volatile LONG stop;
    BOOL closed, dead;
    BYTE *sx;
    UINT32 sxlen, sxcap;
    LARGE_INTEGER t0, freq;
    UINT dev; WCHAR raw[MAXPNAMELEN + 1]; LONG epoch; BOOL hopen, gone;
};

static void CALLBACK mip_cb(HMIDIIN h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    struct mip *m = (struct mip *)inst;
    struct qitem it = {0};
    if (!m) return;
    it.h = h;
    if (msg == MIM_DATA) { it.kind = 0; it.p1 = (DWORD)p1; }
    else if (msg == MIM_LONGDATA) { it.kind = 1; it.hdr = (MIDIHDR *)p1; }
    else return;
    EnterCriticalSection(&m->qcs);
    if ((m->qt + 1) % QLEN != m->qh)
    {
        m->q[m->qt] = it;
        m->qt = (m->qt + 1) % QLEN;
    }
    LeaveCriticalSection(&m->qcs);
    SetEvent(m->qev);
}

static void mip_dispatch(struct mip *m, const BYTE *d, UINT32 n)
{
    struct mmsg *msg;
    struct margs *args;
    void *hs[MAXH];
    int nh, k;
    LARGE_INTEGER now;
    if (!n) return;
    msg = calloc(1, sizeof(*msg));
    obj_init(&msg->o, &type_mmsg, vt_mmsg, NULL, NULL);
    msg->buf = buffer_create(d, n);
    QueryPerformanceCounter(&now);
    msg->ts = (INT64)((double)(now.QuadPart - m->t0.QuadPart) * 1e7 / (double)m->freq.QuadPart);
    msg->type = d[0] >= 0xf0 ? d[0] : (d[0] & 0xf0);
    args = calloc(1, sizeof(*args));
    obj_init(&args->o, &type_margs, vt_margs, NULL, NULL);
    args->m = msg;

    EnterCriticalSection(&m->cs);
    nh = m->nh;
    for (k = 0; k < nh; k++) { hs[k] = m->handlers[k].h; com_addref(hs[k]); }
    LeaveCriticalSection(&m->cs);

    if (verbose()) LOG("IN %u bytes -> %d handler(s): %s", n, nh, dbghex(d, n));
    for (k = 0; k < nh; k++)
    {
        HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, void *))VT(hs[k])[3])(hs[k], &m->o.ifs[0],
                                                                                         &args->o.ifs[0]);
        if (FAILED(hr)) LOG("handler retornou %08lx", hr);
        com_release(hs[k]);
    }
    obj_Release(&args->o.ifs[0]);
}

static void mip_sysex_feed(struct mip *m, const BYTE *d, UINT32 n)
{
    UINT32 k;
    for (k = 0; k < n; k++)
    {
        BYTE b = d[k];
        if (b == 0xf0) m->sxlen = 0;
        else if (!m->sxlen) continue; /* lixo fora de um sysex */
        if (m->sxlen >= m->sxcap)
        {
            m->sxcap = m->sxcap ? m->sxcap * 2 : 4096;
            if (m->sxcap > 16 * 1024 * 1024) { m->sxlen = 0; m->sxcap = 4096; }
            m->sx = realloc(m->sx, m->sxcap);
        }
        m->sx[m->sxlen++] = b;
        if (b == 0xf7)
        {
            mip_dispatch(m, m->sx, m->sxlen);
            m->sxlen = 0;
        }
    }
}

static void mip_teardown(struct mip *m); /* fecha winmm (não espera a thread) */

/* fecha só o handle winmm (buffers incluídos). Só na thread da porta ou depois que ela parou. */
static void mip_close_handle(struct mip *m)
{
    int k;
    if (!m->hopen) return;
    m->hopen = FALSE;
    midiInStop(m->h);
    midiInReset(m->h);
    for (k = 0; k < NBUF; k++) midiInUnprepareHeader(m->h, &m->hdrs[k], sizeof(m->hdrs[k]));
    midiInClose(m->h);
}

static MMRESULT mip_open_handle(struct mip *m)
{
    MMRESULT r = midiInOpen(&m->h, m->dev, (DWORD_PTR)mip_cb, (DWORD_PTR)m, CALLBACK_FUNCTION);
    int k;
    if (r != MMSYSERR_NOERROR) return r;
    for (k = 0; k < NBUF; k++)
    {
        if (!m->hdrs[k].lpData) m->hdrs[k].lpData = malloc(BUFSZ);
        m->hdrs[k].dwBufferLength = BUFSZ;
        m->hdrs[k].dwBytesRecorded = 0;
        m->hdrs[k].dwFlags = 0;
        midiInPrepareHeader(m->h, &m->hdrs[k], sizeof(m->hdrs[k]));
        midiInAddBuffer(m->h, &m->hdrs[k], sizeof(m->hdrs[k]));
    }
    midiInStart(m->h);
    m->hopen = TRUE;
    return r;
}

/* na thread da porta: percebe saída/volta da pedaleira e reabre o handle */
static void mip_hotplug(struct mip *m)
{
    LONG e = port_epoch(m->raw);
    MMRESULT r;
    if (m->closed || m->stop) return;
    if (!e)
    {
        if (!m->gone) LOG("MidiInPort: %s desconectada, esperando voltar", dbgw(m->raw));
        m->gone = TRUE;
        return;
    }
    if (e == m->epoch && m->hopen) return;
    mip_close_handle(m);
    EnterCriticalSection(&m->qcs);
    m->qh = m->qt; /* descarta o que veio do handle antigo */
    LeaveCriticalSection(&m->qcs);
    m->sxlen = 0;
    r = mip_open_handle(m);
    if (r != MMSYSERR_NOERROR)
    {
        LOG("MidiInPort: reabrir %s falhou (%u), tentando de novo", dbgw(m->raw), r);
        return;
    }
    m->epoch = e;
    m->gone = FALSE;
    LOG("MidiInPort: %s reconectada", dbgw(m->raw));
}

static DWORD WINAPI mip_thread(void *arg)
{
    struct mip *m = arg;
    for (;;)
    {
        WaitForSingleObject(m->qev, 50);
        mip_hotplug(m);
        for (;;)
        {
            struct qitem it;
            BOOL have = FALSE;
            EnterCriticalSection(&m->qcs);
            if (m->qh != m->qt)
            {
                it = m->q[m->qh];
                m->qh = (m->qh + 1) % QLEN;
                have = TRUE;
            }
            LeaveCriticalSection(&m->qcs);
            if (!have || m->stop) break;
            if (it.h != m->h || !m->hopen) continue; /* sobra de um handle já fechado */

            obj_AddRef(&m->o.ifs[0]);
            if (it.kind == 0)
            {
                BYTE b[3];
                BYTE st = it.p1 & 0xff;
                int len = st >= 0x80 ? short_len(st) : 1, k;
                for (k = 0; k < len; k++) b[k] = (it.p1 >> (8 * k)) & 0xff;
                mip_dispatch(m, b, len);
            }
            else
            {
                MIDIHDR *h = it.hdr;
                if (h->dwBytesRecorded) mip_sysex_feed(m, (BYTE *)h->lpData, h->dwBytesRecorded);
                if (!m->stop && !m->closed)
                {
                    h->dwBytesRecorded = 0;
                    midiInAddBuffer(m->h, h, sizeof(*h));
                }
            }
            obj_Release(&m->o.ifs[0]);
            if (m->dead) break;
        }
        if (m->dead)
        {
            /* última referência foi solta dentro de um handler: limpa aqui */
            mip_teardown(m);
            CloseHandle(m->thread);
            CloseHandle(m->qev);
            DeleteCriticalSection(&m->cs);
            DeleteCriticalSection(&m->qcs);
            pWindowsDeleteString(m->id);
            free(m->sx);
            obj_free_base(&m->o);
            free(m);
            return 0;
        }
        if (m->stop) return 0;
    }
}

static void mip_teardown(struct mip *m)
{
    int k;
    EnterCriticalSection(&m->cs);
    if (m->closed) { LeaveCriticalSection(&m->cs); return; }
    m->closed = TRUE;
    LeaveCriticalSection(&m->cs);
    mip_close_handle(m);
    for (k = 0; k < NBUF; k++)
    {
        free(m->hdrs[k].lpData);
        m->hdrs[k].lpData = NULL;
    }
    EnterCriticalSection(&m->cs);
    for (k = 0; k < m->nh; k++) com_release(m->handlers[k].h);
    m->nh = 0;
    LeaveCriticalSection(&m->cs);
    LOG("MidiInPort fechada %s", dbgw(hs_raw(m->id)));
}

/* para a thread (espera se não for ela mesma) e fecha o winmm */
static void mip_close(object *o)
{
    struct mip *m = (struct mip *)o;
    InterlockedExchange(&m->stop, 1);
    SetEvent(m->qev);
    if (GetCurrentThreadId() != m->tid) WaitForSingleObject(m->thread, 3000);
    mip_teardown(m);
}

static void mip_destroy(object *o)
{
    struct mip *m = (struct mip *)o;
    if (GetCurrentThreadId() == m->tid) { m->dead = TRUE; return; } /* a thread limpa depois */
    mip_close(o);
    CloseHandle(m->thread);
    CloseHandle(m->qev);
    DeleteCriticalSection(&m->cs);
    DeleteCriticalSection(&m->qcs);
    pWindowsDeleteString(m->id);
    free(m->sx);
    obj_free_base(o);
    free(m);
}

static HRESULT STDMETHODCALLTYPE mip_add_MessageReceived(iface *i, void *handler, INT64 *token)
{
    struct mip *m = OUTER(struct mip, i);
    HRESULT hr = S_OK;
    if (!handler) return E_POINTER;
    EnterCriticalSection(&m->cs);
    if (m->nh >= MAXH) hr = E_OUTOFMEMORY;
    else
    {
        com_addref(handler);
        m->handlers[m->nh].h = handler;
        m->handlers[m->nh].token = *token = ++m->next_token;
        m->nh++;
    }
    LeaveCriticalSection(&m->cs);
    LOG("MessageReceived += token %lld", (long long)*token);
    return hr;
}

static HRESULT STDMETHODCALLTYPE mip_remove_MessageReceived(iface *i, INT64 token)
{
    struct mip *m = OUTER(struct mip, i);
    int k;
    void *h = NULL;
    EnterCriticalSection(&m->cs);
    for (k = 0; k < m->nh; k++)
    {
        if (m->handlers[k].token == token)
        {
            h = m->handlers[k].h;
            memmove(&m->handlers[k], &m->handlers[k + 1], (m->nh - k - 1) * sizeof(m->handlers[0]));
            m->nh--;
            break;
        }
    }
    LeaveCriticalSection(&m->cs);
    if (h) com_release(h);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mip_get_DeviceId(iface *i, HSTRG *v) { *v = hs_dup(OUTER(struct mip, i)->id); return S_OK; }

static void *const vt_mip[] = {INSP, F(mip_add_MessageReceived), F(mip_remove_MessageReceived), F(mip_get_DeviceId)};
static const objtype type_mip = {"MidiInPort", L"Windows.Devices.Midi.MidiInPort", mip_destroy, mip_close, NULL,
                                 {{&IID_IMidiInPort_, 0}, {&IID_IClosable_, 1}}};

static HRESULT mip_open(HSTRG id, struct mip **out)
{
    struct portinfo p;
    struct mip *m;
    MMRESULT r;
    if (!find_port(hs_raw(id), &p) || !p.in) { LOG("MidiInPort.FromIdAsync: id desconhecido %s", dbgw(hs_raw(id))); return E_INVALIDARG; }
    m = calloc(1, sizeof(*m));
    obj_init(&m->o, &type_mip, vt_mip, vt_closable, NULL);
    InitializeCriticalSection(&m->cs);
    InitializeCriticalSection(&m->qcs);
    m->qev = CreateEventW(NULL, FALSE, FALSE, NULL);
    m->id = hs_dup(id);
    QueryPerformanceFrequency(&m->freq);
    QueryPerformanceCounter(&m->t0);
    m->dev = p.dev;
    wcscpy(m->raw, p.raw);
    m->epoch = port_epoch(p.raw);
    r = mip_open_handle(m);
    if (r != MMSYSERR_NOERROR)
    {
        LOG("midiInOpen(%u) falhou: %u", p.dev, r);
        CloseHandle(m->qev);
        DeleteCriticalSection(&m->cs);
        DeleteCriticalSection(&m->qcs);
        pWindowsDeleteString(m->id);
        free(m);
        return E_ACCESSDENIED;
    }
    m->thread = CreateThread(NULL, 0, mip_thread, m, 0, &m->tid);
    LOG("MidiInPort aberta: %s (winmm %u)", dbgw(p.raw), p.dev);
    *out = m;
    return S_OK;
}

/* ---------------------------------------------------- DataWriter / Reader */

struct writer { object o; BYTE *d; UINT32 len, cap; int order, enc; };

static void writer_destroy(object *o)
{
    struct writer *w = (struct writer *)o;
    free(w->d);
    obj_free_base(o);
    free(w);
}
static void w_put(struct writer *w, const void *p, UINT32 n)
{
    if (w->len + n > w->cap)
    {
        while (w->len + n > w->cap) w->cap = w->cap ? w->cap * 2 : 256;
        w->d = realloc(w->d, w->cap);
    }
    memcpy(w->d + w->len, p, n);
    w->len += n;
}
static void w_int(struct writer *w, UINT64 v, int n)
{
    BYTE b[8];
    int k;
    for (k = 0; k < n; k++) b[w->order ? n - 1 - k : k] = (v >> (8 * k)) & 0xff;
    w_put(w, b, n);
}
#define WR(i) OUTER(struct writer, i)
static HRESULT STDMETHODCALLTYPE w_get_Unstored(iface *i, UINT32 *v) { *v = WR(i)->len; return S_OK; }
static HRESULT STDMETHODCALLTYPE w_get_Enc(iface *i, int *v) { *v = WR(i)->enc; return S_OK; }
static HRESULT STDMETHODCALLTYPE w_put_Enc(iface *i, int v) { WR(i)->enc = v; return S_OK; }
static HRESULT STDMETHODCALLTYPE w_get_Order(iface *i, int *v) { *v = WR(i)->order; return S_OK; }
static HRESULT STDMETHODCALLTYPE w_put_Order(iface *i, int v) { WR(i)->order = v; return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteByte(iface *i, BYTE v) { w_put(WR(i), &v, 1); return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteBytes(iface *i, UINT32 n, const BYTE *p) { w_put(WR(i), p, n); return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteBuffer(iface *i, void *buf)
{
    BYTE *d; UINT32 n;
    HRESULT hr = ibuffer_bytes(buf, &d, &n);
    if (SUCCEEDED(hr)) w_put(WR(i), d, n);
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_WriteBufferRange(iface *i, void *buf, UINT32 start, UINT32 count)
{
    BYTE *d; UINT32 n;
    HRESULT hr = ibuffer_bytes(buf, &d, &n);
    if (FAILED(hr)) return hr;
    if (start > n || count > n - start) return E_BOUNDS;
    w_put(WR(i), d + start, count);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_WriteBoolean(iface *i, BYTE v) { BYTE b = !!v; w_put(WR(i), &b, 1); return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteI16(iface *i, INT16 v) { w_int(WR(i), (UINT16)v, 2); return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteI32(iface *i, INT32 v) { w_int(WR(i), (UINT32)v, 4); return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteI64(iface *i, INT64 v) { w_int(WR(i), (UINT64)v, 8); return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteSingle(iface *i, float v) { UINT32 u; memcpy(&u, &v, 4); w_int(WR(i), u, 4); return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteDouble(iface *i, double v) { UINT64 u; memcpy(&u, &v, 8); w_int(WR(i), u, 8); return S_OK; }
static HRESULT STDMETHODCALLTYPE w_WriteString(iface *i, HSTRG s, UINT32 *count)
{
    const WCHAR *r = hs_raw(s);
    int n = WideCharToMultiByte(CP_UTF8, 0, r, (int)wcslen(r), NULL, 0, NULL, NULL);
    char *b = malloc(n + 1);
    WideCharToMultiByte(CP_UTF8, 0, r, (int)wcslen(r), b, n, NULL, NULL);
    w_put(WR(i), b, n);
    free(b);
    *count = n;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_MeasureString(iface *i, HSTRG s, UINT32 *count)
{
    const WCHAR *r = hs_raw(s);
    *count = WideCharToMultiByte(CP_UTF8, 0, r, (int)wcslen(r), NULL, 0, NULL, NULL);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_DetachBuffer(iface *i, void **out)
{
    struct writer *w = WR(i);
    struct buffer *b = buffer_create(w->d, w->len);
    w->len = 0;
    *out = &b->o.ifs[0];
    return S_OK;
}
static void *const vt_writer[] = {INSP,
    F(w_get_Unstored), F(w_get_Enc), F(w_put_Enc), F(w_get_Order), F(w_put_Order),
    F(w_WriteByte), F(w_WriteBytes), F(w_WriteBuffer), F(w_WriteBufferRange), F(w_WriteBoolean),
    F(notimpl) /* WriteGuid */, F(w_WriteI16), F(w_WriteI32), F(w_WriteI64),
    F(w_WriteI16) /* UInt16 */, F(w_WriteI32) /* UInt32 */, F(w_WriteI64) /* UInt64 */,
    F(w_WriteSingle), F(w_WriteDouble), F(w_WriteI64) /* DateTime */, F(w_WriteI64) /* TimeSpan */,
    F(w_WriteString), F(w_MeasureString), F(notimpl) /* StoreAsync */, F(notimpl) /* FlushAsync */,
    F(w_DetachBuffer), F(notimpl) /* DetachStream */};
static const objtype type_writer = {"DataWriter", L"Windows.Storage.Streams.DataWriter", writer_destroy, NULL, NULL,
                                    {{&IID_IDataWriter_, 0}, {&IID_IClosable_, 1}}};

struct reader { object o; BYTE *d; UINT32 len, pos; int order, enc, opts; };

static void reader_destroy(object *o)
{
    struct reader *r = (struct reader *)o;
    free(r->d);
    obj_free_base(o);
    free(r);
}
#define RD(i) OUTER(struct reader, i)
static HRESULT r_int(struct reader *r, int n, UINT64 *v)
{
    int k;
    *v = 0;
    if (r->len - r->pos < (UINT32)n) return E_BOUNDS;
    for (k = 0; k < n; k++) *v |= (UINT64)r->d[r->pos + (r->order ? n - 1 - k : k)] << (8 * k);
    r->pos += n;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_get_Unconsumed(iface *i, UINT32 *v) { *v = RD(i)->len - RD(i)->pos; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_get_Enc(iface *i, int *v) { *v = RD(i)->enc; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_put_Enc(iface *i, int v) { RD(i)->enc = v; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_get_Order(iface *i, int *v) { *v = RD(i)->order; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_put_Order(iface *i, int v) { RD(i)->order = v; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_get_Opts(iface *i, int *v) { *v = RD(i)->opts; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_put_Opts(iface *i, int v) { RD(i)->opts = v; return S_OK; }
static HRESULT STDMETHODCALLTYPE r_ReadByte(iface *i, BYTE *v)
{
    UINT64 x; HRESULT hr = r_int(RD(i), 1, &x);
    *v = (BYTE)x;
    return hr;
}
static HRESULT STDMETHODCALLTYPE r_ReadBytes(iface *i, UINT32 n, BYTE *out)
{
    struct reader *r = RD(i);
    if (r->len - r->pos < n) return E_BOUNDS;
    memcpy(out, r->d + r->pos, n);
    r->pos += n;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_ReadBuffer(iface *i, UINT32 n, void **out)
{
    struct reader *r = RD(i);
    struct buffer *b;
    if (r->len - r->pos < n) return E_BOUNDS;
    b = buffer_create(r->d + r->pos, n);
    r->pos += n;
    *out = &b->o.ifs[0];
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_ReadBoolean(iface *i, BYTE *v) { HRESULT hr = r_ReadByte(i, v); *v = !!*v; return hr; }
static HRESULT STDMETHODCALLTYPE r_Read16(iface *i, UINT16 *v) { UINT64 x; HRESULT hr = r_int(RD(i), 2, &x); *v = (UINT16)x; return hr; }
static HRESULT STDMETHODCALLTYPE r_Read32(iface *i, UINT32 *v) { UINT64 x; HRESULT hr = r_int(RD(i), 4, &x); *v = (UINT32)x; return hr; }
static HRESULT STDMETHODCALLTYPE r_Read64(iface *i, UINT64 *v) { return r_int(RD(i), 8, v); }
static HRESULT STDMETHODCALLTYPE r_ReadString(iface *i, UINT32 n, HSTRG *out)
{
    struct reader *r = RD(i);
    WCHAR *w;
    int wn;
    if (r->len - r->pos < n) return E_BOUNDS;
    wn = MultiByteToWideChar(CP_UTF8, 0, (char *)r->d + r->pos, n, NULL, 0);
    w = calloc(wn + 1, sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, (char *)r->d + r->pos, n, w, wn);
    r->pos += n;
    *out = hs_new(w);
    free(w);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_DetachBuffer(iface *i, void **out)
{
    struct reader *r = RD(i);
    struct buffer *b = buffer_create(r->d + r->pos, r->len - r->pos);
    r->pos = r->len;
    *out = &b->o.ifs[0];
    return S_OK;
}
static void *const vt_reader[] = {INSP,
    F(r_get_Unconsumed), F(r_get_Enc), F(r_put_Enc), F(r_get_Order), F(r_put_Order), F(r_get_Opts), F(r_put_Opts),
    F(r_ReadByte), F(r_ReadBytes), F(r_ReadBuffer), F(r_ReadBoolean), F(notimpl) /* ReadGuid */,
    F(r_Read16), F(r_Read32), F(r_Read64), F(r_Read16), F(r_Read32), F(r_Read64),
    F(r_Read32) /* Single (bits) */, F(r_Read64) /* Double (bits) */, F(r_ReadString),
    F(r_Read64) /* DateTime */, F(r_Read64) /* TimeSpan */, F(notimpl) /* LoadAsync */,
    F(r_DetachBuffer), F(notimpl) /* DetachStream */};
static const objtype type_reader = {"DataReader", L"Windows.Storage.Streams.DataReader", reader_destroy, NULL, NULL,
                                    {{&IID_IDataReader_, 0}, {&IID_IClosable_, 1}}};

/* ------------------------------------------------------------- fábricas */

enum { FAC_MIDIOUT, FAC_MIDIIN, FAC_DEVINFO, FAC_WRITER, FAC_READER, FAC_COUNT };
struct factory { object o; int kind; };
static struct factory factories[FAC_COUNT];

static void *orig_devinfo_factory, *orig_devinfo_statics;

static HRESULT STDMETHODCALLTYPE fac_ActivateInstance(iface *i, void **out)
{
    struct factory *f = OUTER(struct factory, i);
    *out = NULL;
    if (f->kind == FAC_WRITER)
    {
        struct writer *w = calloc(1, sizeof(*w));
        obj_init(&w->o, &type_writer, vt_writer, vt_closable, NULL);
        w->order = 1; /* BigEndian (padrão do WinRT) */
        *out = &w->o.ifs[0];
        return S_OK;
    }
    if (f->kind == FAC_DEVINFO && orig_devinfo_factory)
        return ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))VT(orig_devinfo_factory)[6])(orig_devinfo_factory, out);
    return E_NOTIMPL;
}
static void *const vt_actfactory[] = {INSP, F(fac_ActivateInstance)};

/* MidiOutPort statics */
#define MIDI_OUT_SELECTOR L"System.Devices.InterfaceClassGuid:=\"{6DC23320-AB33-4CE4-80D4-BBB3EBBF2814}\" AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True"
#define MIDI_IN_SELECTOR  L"System.Devices.InterfaceClassGuid:=\"{504BE32C-CCF6-4D2C-B73F-6F8B3747E22B}\" AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True"

static HRESULT STDMETHODCALLTYPE outs_FromIdAsync(iface *i, HSTRG id, void **op)
{
    struct mop *m = NULL;
    HRESULT hr = mop_open(id, &m);
    *op = asyncop_create(&IID_IAsyncOperation_IMidiOutPort, m ? &m->o.ifs[0] : NULL, hr);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE outs_GetDeviceSelector(iface *i, HSTRG *v) { *v = hs_new(MIDI_OUT_SELECTOR); return S_OK; }
static void *const vt_outstatics[] = {INSP, F(outs_FromIdAsync), F(outs_GetDeviceSelector)};

static HRESULT STDMETHODCALLTYPE ins_FromIdAsync(iface *i, HSTRG id, void **op)
{
    struct mip *m = NULL;
    HRESULT hr = mip_open(id, &m);
    *op = asyncop_create(&IID_IAsyncOperation_MidiInPort, m ? &m->o.ifs[0] : NULL, hr);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ins_GetDeviceSelector(iface *i, HSTRG *v) { *v = hs_new(MIDI_IN_SELECTOR); return S_OK; }
static void *const vt_instatics[] = {INSP, F(ins_FromIdAsync), F(ins_GetDeviceSelector)};

/* DeviceInformation statics (proxy) */
static int midi_selector_kind(HSTRG filter) /* 0 = não é MIDI, 1 = in, 2 = out */
{
    const WCHAR *s = hs_raw(filter);
    WCHAR up[1024];
    size_t k;
    for (k = 0; s[k] && k < 1023; k++) up[k] = towupper(s[k]);
    up[k] = 0;
    if (wcsstr(up, L"504BE32C-CCF6-4D2C-B73F-6F8B3747E22B")) return 1;
    if (wcsstr(up, L"6DC23320-AB33-4CE4-80D4-BBB3EBBF2814")) return 2;
    return 0;
}

typedef HRESULT (STDMETHODCALLTYPE *fn_p1)(void *, void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *fn_p2)(void *, void *, void *, void **);
#define ORIG(idx) (orig_devinfo_statics ? VT(orig_devinfo_statics)[idx] : NULL)

static HRESULT di_from_id(HSTRG id, void **op)
{
    struct portinfo p;
    if (!find_port(hs_raw(id), &p))
    {
        LOG("CreateFromIdAsync: id MIDI desconhecido %s", dbgw(hs_raw(id)));
        *op = asyncop_create(&IID_IAsyncOperation_DeviceInformation, NULL, E_INVALIDARG);
        return S_OK;
    }
    *op = asyncop_create(&IID_IAsyncOperation_DeviceInformation, &devinfo_create(&p)->o.ifs[0], S_OK);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dis_CreateFromIdAsync(iface *i, HSTRG id, void **op)
{
    if (!wcsncmp(hs_raw(id), ID_PREFIX, wcslen(ID_PREFIX))) return di_from_id(id, op);
    if (!ORIG(6)) return E_NOTIMPL;
    return ((fn_p1)ORIG(6))(orig_devinfo_statics, id, op);
}
static HRESULT STDMETHODCALLTYPE dis_CreateFromIdAsyncAP(iface *i, HSTRG id, void *props, void **op)
{
    if (!wcsncmp(hs_raw(id), ID_PREFIX, wcslen(ID_PREFIX))) return di_from_id(id, op);
    if (!ORIG(7)) return E_NOTIMPL;
    return ((fn_p2)ORIG(7))(orig_devinfo_statics, id, props, op);
}
static HRESULT STDMETHODCALLTYPE dis_FindAllAsync(iface *i, void **op)
{
    if (!ORIG(8)) return E_NOTIMPL;
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))ORIG(8))(orig_devinfo_statics, op);
}
static HRESULT STDMETHODCALLTYPE dis_FindAllAsyncDeviceClass(iface *i, int cls, void **op)
{
    if (!ORIG(9)) return E_NOTIMPL;
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, int, void **))ORIG(9))(orig_devinfo_statics, cls, op);
}
static HRESULT find_midi(int kind, void **op)
{
    struct devcoll *c = devcoll_create(kind == 1);
    static LONG last[3] = {-1, -1, -1};
    if (InterlockedExchange(&last[kind], (LONG)c->n) != (LONG)c->n || verbose())
    {
        UINT32 k;
        LOG("FindAll MIDI %s: %u porta(s)", kind == 1 ? "IN" : "OUT", c->n);
        for (k = 0; k < c->n; k++)
            LOG("   [%u] %s  id=%s", k, dbgw(hs_raw(c->items[k]->name)), dbgw(hs_raw(c->items[k]->id)));
    }
    *op = asyncop_create(&IID_IAsyncOperation_DeviceInformationCollection, &c->o.ifs[0], S_OK);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dis_FindAllAsyncAqsFilter(iface *i, HSTRG filter, void **op)
{
    int k = midi_selector_kind(filter);
    if (k) return find_midi(k, op);
    if (!ORIG(10)) return E_NOTIMPL;
    return ((fn_p1)ORIG(10))(orig_devinfo_statics, filter, op);
}
static HRESULT STDMETHODCALLTYPE dis_FindAllAsyncAqsFilterAP(iface *i, HSTRG filter, void *props, void **op)
{
    int k = midi_selector_kind(filter);
    if (k) return find_midi(k, op);
    if (!ORIG(11)) return E_NOTIMPL;
    return ((fn_p2)ORIG(11))(orig_devinfo_statics, filter, props, op);
}
static HRESULT STDMETHODCALLTYPE dis_CreateWatcher(iface *i, void **w)
{
    if (!ORIG(12)) return E_NOTIMPL;
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))ORIG(12))(orig_devinfo_statics, w);
}
static HRESULT STDMETHODCALLTYPE dis_CreateWatcherDeviceClass(iface *i, int cls, void **w)
{
    if (!ORIG(13)) return E_NOTIMPL;
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, int, void **))ORIG(13))(orig_devinfo_statics, cls, w);
}
static HRESULT STDMETHODCALLTYPE dis_CreateWatcherAqsFilter(iface *i, HSTRG filter, void **w)
{
    if (midi_selector_kind(filter)) LOG("AVISO: CreateWatcher com seletor MIDI (não suportado, repassando)");
    if (!ORIG(14)) return E_NOTIMPL;
    return ((fn_p1)ORIG(14))(orig_devinfo_statics, filter, w);
}
static HRESULT STDMETHODCALLTYPE dis_CreateWatcherAqsFilterAP(iface *i, HSTRG filter, void *props, void **w)
{
    if (midi_selector_kind(filter)) LOG("AVISO: CreateWatcher com seletor MIDI (não suportado, repassando)");
    if (!ORIG(15)) return E_NOTIMPL;
    return ((fn_p2)ORIG(15))(orig_devinfo_statics, filter, props, w);
}
static void *const vt_distatics[] = {INSP, F(dis_CreateFromIdAsync), F(dis_CreateFromIdAsyncAP), F(dis_FindAllAsync),
                                     F(dis_FindAllAsyncDeviceClass), F(dis_FindAllAsyncAqsFilter),
                                     F(dis_FindAllAsyncAqsFilterAP), F(dis_CreateWatcher),
                                     F(dis_CreateWatcherDeviceClass), F(dis_CreateWatcherAqsFilter),
                                     F(dis_CreateWatcherAqsFilterAP)};

static HRESULT devinfo_fac_fallback(object *o, const GUID *iid, void **out)
{
    if (orig_devinfo_factory) return com_qi(orig_devinfo_factory, iid, out);
    return E_NOINTERFACE;
}

/* DataReader statics */
static HRESULT STDMETHODCALLTYPE rds_FromBuffer(iface *i, void *buf, void **out)
{
    BYTE *d; UINT32 n;
    struct reader *r;
    HRESULT hr = ibuffer_bytes(buf, &d, &n);
    if (FAILED(hr)) return hr;
    r = calloc(1, sizeof(*r));
    obj_init(&r->o, &type_reader, vt_reader, vt_closable, NULL);
    r->order = 1;
    r->len = n;
    r->d = malloc(n ? n : 1);
    memcpy(r->d, d, n);
    *out = &r->o.ifs[0];
    return S_OK;
}
static void *const vt_rdstatics[] = {INSP, F(rds_FromBuffer)};

static const objtype type_fac_out = {"MidiOutPortFactory", L"Windows.Devices.Midi.MidiOutPort", NULL, NULL, NULL,
                                     {{&IID_IActivationFactory_, 0}, {&IID_IMidiOutPortStatics_, 1}}};
static const objtype type_fac_in = {"MidiInPortFactory", L"Windows.Devices.Midi.MidiInPort", NULL, NULL, NULL,
                                    {{&IID_IActivationFactory_, 0}, {&IID_IMidiInPortStatics_, 1}}};
static const objtype type_fac_di = {"DeviceInformationFactory", L"Windows.Devices.Enumeration.DeviceInformation", NULL,
                                    NULL, devinfo_fac_fallback,
                                    {{&IID_IActivationFactory_, 0}, {&IID_IDeviceInformationStatics_, 1}}};
static const objtype type_fac_wr = {"DataWriterFactory", L"Windows.Storage.Streams.DataWriter", NULL, NULL, NULL,
                                    {{&IID_IActivationFactory_, 0}}};
static const objtype type_fac_rd = {"DataReaderFactory", L"Windows.Storage.Streams.DataReader", NULL, NULL, NULL,
                                    {{&IID_IActivationFactory_, 0}, {&IID_IDataReaderStatics_, 1}}};

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK do_init(INIT_ONCE *io, void *p, void **ctx)
{
    HMODULE combase = GetModuleHandleW(L"combase.dll");
    HMODULE en;
    if (!combase) combase = LoadLibraryW(L"combase.dll");
    pWindowsCreateString = (void *)GetProcAddress(combase, "WindowsCreateString");
    pWindowsDeleteString = (void *)GetProcAddress(combase, "WindowsDeleteString");
    pWindowsGetStringRawBuffer = (void *)GetProcAddress(combase, "WindowsGetStringRawBuffer");
    pWindowsDuplicateString = (void *)GetProcAddress(combase, "WindowsDuplicateString");

    obj_init(&factories[FAC_MIDIOUT].o, &type_fac_out, vt_actfactory, vt_outstatics, NULL);
    obj_init(&factories[FAC_MIDIIN].o, &type_fac_in, vt_actfactory, vt_instatics, NULL);
    obj_init(&factories[FAC_DEVINFO].o, &type_fac_di, vt_actfactory, vt_distatics, NULL);
    obj_init(&factories[FAC_WRITER].o, &type_fac_wr, vt_actfactory, NULL, NULL);
    obj_init(&factories[FAC_READER].o, &type_fac_rd, vt_actfactory, vt_rdstatics, NULL);
    factories[FAC_MIDIOUT].kind = FAC_MIDIOUT;
    factories[FAC_MIDIIN].kind = FAC_MIDIIN;
    factories[FAC_DEVINFO].kind = FAC_DEVINFO;
    factories[FAC_WRITER].kind = FAC_WRITER;
    factories[FAC_READER].kind = FAC_READER;

    en = LoadLibraryW(L"windows.devices.enumeration.dll");
    if (en)
    {
        HRESULT (WINAPI *get)(HSTRG, void **) = (void *)GetProcAddress(en, "DllGetActivationFactory");
        HSTRG cls = hs_new(L"Windows.Devices.Enumeration.DeviceInformation");
        if (get && SUCCEEDED(get(cls, &orig_devinfo_factory)))
            com_qi(orig_devinfo_factory, &IID_IDeviceInformationStatics_, &orig_devinfo_statics);
        pWindowsDeleteString(cls);
    }
    LOG("winrtmidi carregada (DeviceInformation original: %p / %p)", orig_devinfo_factory, orig_devinfo_statics);
    return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRG classid, void **factory)
{
    static const struct { const WCHAR *name; int kind; } map[] = {
        {L"Windows.Devices.Midi.MidiOutPort", FAC_MIDIOUT},
        {L"Windows.Devices.Midi.MidiInPort", FAC_MIDIIN},
        {L"Windows.Devices.Enumeration.DeviceInformation", FAC_DEVINFO},
        {L"Windows.Storage.Streams.DataWriter", FAC_WRITER},
        {L"Windows.Storage.Streams.DataReader", FAC_READER},
    };
    const WCHAR *name;
    size_t k;
    InitOnceExecuteOnce(&init_once, do_init, NULL, NULL);
    name = hs_raw(classid);
    for (k = 0; k < sizeof(map) / sizeof(map[0]); k++)
    {
        if (!wcscmp(name, map[k].name))
        {
            *factory = &factories[map[k].kind].o.ifs[0];
            obj_AddRef(*factory);
            LOG("DllGetActivationFactory(%s) -> ok", dbgw(name));
            return S_OK;
        }
    }
    LOG("DllGetActivationFactory(%s) -> não disponível", dbgw(name));
    *factory = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        InitializeCriticalSection(&log_cs);
        InitializeCriticalSection(&pres_cs);
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
