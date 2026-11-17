// dispositivo MIDI falso pro teste de hotplug: cliente "FakeNano", porta "FakeNano MIDI 1", faz eco
#include <alsa/asoundlib.h>
int main(void) {
    snd_seq_t *s;
    if (snd_seq_open(&s, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return 1;
    snd_seq_set_client_name(s, "FakeNano");
    snd_seq_set_client_pool_input(s, 2000); snd_seq_set_client_pool_output(s, 2000);
    int p = snd_seq_create_simple_port(s, "FakeNano MIDI 1",
        SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ|SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_HARDWARE);
    fprintf(stderr, "fakedev cliente %d porta %d\n", snd_seq_client_id(s), p);
    for (;;) {
        snd_seq_event_t *ev;
        int r = snd_seq_event_input(s, &ev);
        if (r == -ENOSPC) continue;
        if (r < 0) break;
        if (ev->type >= SND_SEQ_EVENT_CLIENT_START && ev->type <= SND_SEQ_EVENT_PORT_UNSUBSCRIBED) continue;
        snd_seq_ev_set_direct(ev); snd_seq_ev_set_source(ev, p); snd_seq_ev_set_subs(ev);
        snd_seq_event_output_direct(s, ev);
    }
    return 0;
}
