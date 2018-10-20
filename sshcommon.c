/*
 * Supporting routines used in common by all the various components of
 * the SSH system.
 */

#include <assert.h>
#include <stdlib.h>

#include "putty.h"
#include "ssh.h"
#include "sshbpp.h"
#include "sshppl.h"
#include "sshchan.h"

/* ----------------------------------------------------------------------
 * Implementation of PacketQueue.
 */

static void pq_ensure_unlinked(PacketQueueNode *node)
{
    if (node->on_free_queue) {
        node->next->prev = node->prev;
        node->prev->next = node->next;
    } else {
        assert(!node->next);
        assert(!node->prev);
    }
}

void pq_base_push(PacketQueueBase *pqb, PacketQueueNode *node)
{
    pq_ensure_unlinked(node);
    node->next = &pqb->end;
    node->prev = pqb->end.prev;
    node->next->prev = node;
    node->prev->next = node;

    if (pqb->ic)
        queue_idempotent_callback(pqb->ic);
}

void pq_base_push_front(PacketQueueBase *pqb, PacketQueueNode *node)
{
    pq_ensure_unlinked(node);
    node->prev = &pqb->end;
    node->next = pqb->end.next;
    node->next->prev = node;
    node->prev->next = node;

    if (pqb->ic)
        queue_idempotent_callback(pqb->ic);
}

static PacketQueueNode pktin_freeq_head = {
    &pktin_freeq_head, &pktin_freeq_head, TRUE
};

static void pktin_free_queue_callback(void *vctx)
{
    while (pktin_freeq_head.next != &pktin_freeq_head) {
        PacketQueueNode *node = pktin_freeq_head.next;
        PktIn *pktin = container_of(node, PktIn, qnode);
        pktin_freeq_head.next = node->next;
        sfree(pktin);
    }

    pktin_freeq_head.prev = &pktin_freeq_head;
}

static IdempotentCallback ic_pktin_free = {
    pktin_free_queue_callback, NULL, FALSE
};

static PktIn *pq_in_after(PacketQueueBase *pqb,
                          PacketQueueNode *prev, int pop)
{
    PacketQueueNode *node = prev->next;
    if (node == &pqb->end)
        return NULL;

    if (pop) {
        node->next->prev = node->prev;
        node->prev->next = node->next;

        node->prev = pktin_freeq_head.prev;
        node->next = &pktin_freeq_head;
        node->next->prev = node;
        node->prev->next = node;
        node->on_free_queue = TRUE;
        queue_idempotent_callback(&ic_pktin_free);
    }

    return container_of(node, PktIn, qnode);
}

static PktOut *pq_out_after(PacketQueueBase *pqb,
                            PacketQueueNode *prev, int pop)
{
    PacketQueueNode *node = prev->next;
    if (node == &pqb->end)
        return NULL;

    if (pop) {
        node->next->prev = node->prev;
        node->prev->next = node->next;
        node->prev = node->next = NULL;
    }

    return container_of(node, PktOut, qnode);
}

void pq_in_init(PktInQueue *pq)
{
    pq->pqb.ic = NULL;
    pq->pqb.end.next = pq->pqb.end.prev = &pq->pqb.end;
    pq->after = pq_in_after;
}

void pq_out_init(PktOutQueue *pq)
{
    pq->pqb.ic = NULL;
    pq->pqb.end.next = pq->pqb.end.prev = &pq->pqb.end;
    pq->after = pq_out_after;
}

void pq_in_clear(PktInQueue *pq)
{
    PktIn *pkt;
    pq->pqb.ic = NULL;
    while ((pkt = pq_pop(pq)) != NULL) {
        /* No need to actually free these packets: pq_pop on a
         * PktInQueue will automatically move them to the free
         * queue. */
    }
}

void pq_out_clear(PktOutQueue *pq)
{
    PktOut *pkt;
    pq->pqb.ic = NULL;
    while ((pkt = pq_pop(pq)) != NULL)
        ssh_free_pktout(pkt);
}

/*
 * Concatenate the contents of the two queues q1 and q2, and leave the
 * result in qdest. qdest must be either empty, or one of the input
 * queues.
 */
void pq_base_concatenate(PacketQueueBase *qdest,
                         PacketQueueBase *q1, PacketQueueBase *q2)
{
    struct PacketQueueNode *head1, *tail1, *head2, *tail2;

    /*
     * Extract the contents from both input queues, and empty them.
     */

    head1 = (q1->end.next == &q1->end ? NULL : q1->end.next);
    tail1 = (q1->end.prev == &q1->end ? NULL : q1->end.prev);
    head2 = (q2->end.next == &q2->end ? NULL : q2->end.next);
    tail2 = (q2->end.prev == &q2->end ? NULL : q2->end.prev);

    q1->end.next = q1->end.prev = &q1->end;
    q2->end.next = q2->end.prev = &q2->end;

    /*
     * Link the two lists together, handling the case where one or
     * both is empty.
     */

    if (tail1)
        tail1->next = head2;
    else
        head1 = head2;

    if (head2)
        head2->prev = tail1;
    else
        tail2 = tail1;

    /*
     * Check the destination queue is currently empty. (If it was one
     * of the input queues, then it will be, because we emptied both
     * of those just a moment ago.)
     */

    assert(qdest->end.next == &qdest->end);
    assert(qdest->end.prev == &qdest->end);

    /*
     * If our concatenated list has anything in it, then put it in
     * dest.
     */

    if (!head1) {
        assert(!tail2);
    } else {
        assert(tail2);
        qdest->end.next = head1;
        qdest->end.prev = tail2;
        head1->prev = &qdest->end;
        tail2->next = &qdest->end;

        if (qdest->ic)
            queue_idempotent_callback(qdest->ic);
    }
}

/* ----------------------------------------------------------------------
 * Low-level functions for the packet structures themselves.
 */

static void ssh_pkt_BinarySink_write(BinarySink *bs,
                                     const void *data, size_t len);
PktOut *ssh_new_packet(void)
{
    PktOut *pkt = snew(PktOut);

    BinarySink_INIT(pkt, ssh_pkt_BinarySink_write);
    pkt->data = NULL;
    pkt->length = 0;
    pkt->maxlen = 0;
    pkt->downstream_id = 0;
    pkt->additional_log_text = NULL;
    pkt->qnode.next = pkt->qnode.prev = NULL;
    pkt->qnode.on_free_queue = FALSE;

    return pkt;
}

static void ssh_pkt_ensure(PktOut *pkt, int length)
{
    if (pkt->maxlen < length) {
        pkt->maxlen = length + 256;
        pkt->data = sresize(pkt->data, pkt->maxlen, unsigned char);
    }
}
static void ssh_pkt_adddata(PktOut *pkt, const void *data, int len)
{
    pkt->length += len;
    ssh_pkt_ensure(pkt, pkt->length);
    memcpy(pkt->data + pkt->length - len, data, len);
}

static void ssh_pkt_BinarySink_write(BinarySink *bs,
                                     const void *data, size_t len)
{
    PktOut *pkt = BinarySink_DOWNCAST(bs, PktOut);
    ssh_pkt_adddata(pkt, data, len);
}

void ssh_free_pktout(PktOut *pkt)
{
    sfree(pkt->data);
    sfree(pkt);
}

/* ----------------------------------------------------------------------
 * Implement zombiechan_new() and its trivial vtable.
 */

static void zombiechan_free(Channel *chan);
static int zombiechan_send(Channel *chan, int is_stderr, const void *, int);
static void zombiechan_set_input_wanted(Channel *chan, int wanted);
static void zombiechan_do_nothing(Channel *chan);
static void zombiechan_open_failure(Channel *chan, const char *);
static int zombiechan_want_close(Channel *chan, int sent_eof, int rcvd_eof);
static char *zombiechan_log_close_msg(Channel *chan) { return NULL; }

static const struct ChannelVtable zombiechan_channelvt = {
    zombiechan_free,
    zombiechan_do_nothing,             /* open_confirmation */
    zombiechan_open_failure,
    zombiechan_send,
    zombiechan_do_nothing,             /* send_eof */
    zombiechan_set_input_wanted,
    zombiechan_log_close_msg,
    zombiechan_want_close,
    chan_no_exit_status,
    chan_no_exit_signal,
    chan_no_exit_signal_numeric,
    chan_no_run_shell,
    chan_no_run_command,
    chan_no_run_subsystem,
    chan_no_enable_x11_forwarding,
    chan_no_enable_agent_forwarding,
    chan_no_allocate_pty,
    chan_no_set_env,
    chan_no_send_break,
    chan_no_send_signal,
    chan_no_change_window_size,
    chan_no_request_response,
};

Channel *zombiechan_new(void)
{
    Channel *chan = snew(Channel);
    chan->vt = &zombiechan_channelvt;
    chan->initial_fixed_window_size = 0;
    return chan;
}

static void zombiechan_free(Channel *chan)
{
    assert(chan->vt == &zombiechan_channelvt);
    sfree(chan);
}

static void zombiechan_do_nothing(Channel *chan)
{
    assert(chan->vt == &zombiechan_channelvt);
}

static void zombiechan_open_failure(Channel *chan, const char *errtext)
{
    assert(chan->vt == &zombiechan_channelvt);
}

static int zombiechan_send(Channel *chan, int is_stderr,
                           const void *data, int length)
{
    assert(chan->vt == &zombiechan_channelvt);
    return 0;
}

static void zombiechan_set_input_wanted(Channel *chan, int enable)
{
    assert(chan->vt == &zombiechan_channelvt);
}

static int zombiechan_want_close(Channel *chan, int sent_eof, int rcvd_eof)
{
    return TRUE;
}

/* ----------------------------------------------------------------------
 * Centralised standard methods for other channel implementations to
 * borrow.
 */

void chan_remotely_opened_confirmation(Channel *chan)
{
    assert(0 && "this channel type should never receive OPEN_CONFIRMATION");
}

void chan_remotely_opened_failure(Channel *chan, const char *errtext)
{
    assert(0 && "this channel type should never receive OPEN_FAILURE");
}

int chan_default_want_close(
    Channel *chan, int sent_local_eof, int rcvd_remote_eof)
{
    /*
     * Default close policy: we start initiating the CHANNEL_CLOSE
     * procedure as soon as both sides of the channel have seen EOF.
     */
    return sent_local_eof && rcvd_remote_eof;
}

int chan_no_exit_status(Channel *chan, int status)
{
    return FALSE;
}

int chan_no_exit_signal(
    Channel *chan, ptrlen signame, int core_dumped, ptrlen msg)
{
    return FALSE;
}

int chan_no_exit_signal_numeric(
    Channel *chan, int signum, int core_dumped, ptrlen msg)
{
    return FALSE;
}

int chan_no_run_shell(Channel *chan)
{
    return FALSE;
}

int chan_no_run_command(Channel *chan, ptrlen command)
{
    return FALSE;
}

int chan_no_run_subsystem(Channel *chan, ptrlen subsys)
{
    return FALSE;
}

int chan_no_enable_x11_forwarding(
    Channel *chan, int oneshot, ptrlen authproto, ptrlen authdata,
    unsigned screen_number)
{
    return FALSE;
}

int chan_no_enable_agent_forwarding(Channel *chan)
{
    return FALSE;
}

int chan_no_allocate_pty(
    Channel *chan, ptrlen termtype, unsigned width, unsigned height,
    unsigned pixwidth, unsigned pixheight, struct ssh_ttymodes modes)
{
    return FALSE;
}

int chan_no_set_env(Channel *chan, ptrlen var, ptrlen value)
{
    return FALSE;
}

int chan_no_send_break(Channel *chan, unsigned length)
{
    return FALSE;
}

int chan_no_send_signal(Channel *chan, ptrlen signame)
{
    return FALSE;
}

int chan_no_change_window_size(
    Channel *chan, unsigned width, unsigned height,
    unsigned pixwidth, unsigned pixheight)
{
    return FALSE;
}

void chan_no_request_response(Channel *chan, int success)
{
    assert(0 && "this channel type should never send a want-reply request");
}

/* ----------------------------------------------------------------------
 * Common routines for handling SSH tty modes.
 */

static unsigned real_ttymode_opcode(unsigned our_opcode, int ssh_version)
{
    switch (our_opcode) {
      case TTYMODE_ISPEED:
        return ssh_version == 1 ? TTYMODE_ISPEED_SSH1 : TTYMODE_ISPEED_SSH2;
      case TTYMODE_OSPEED:
        return ssh_version == 1 ? TTYMODE_OSPEED_SSH1 : TTYMODE_OSPEED_SSH2;
      default:
        return our_opcode;
    }
}

static unsigned our_ttymode_opcode(unsigned real_opcode, int ssh_version)
{
    if (ssh_version == 1) {
        switch (real_opcode) {
          case TTYMODE_ISPEED_SSH1:
            return TTYMODE_ISPEED;
          case TTYMODE_OSPEED_SSH1:
            return TTYMODE_OSPEED;
          default:
            return real_opcode;
        }
    } else {
        switch (real_opcode) {
          case TTYMODE_ISPEED_SSH2:
            return TTYMODE_ISPEED;
          case TTYMODE_OSPEED_SSH2:
            return TTYMODE_OSPEED;
          default:
            return real_opcode;
        }
    }
}

struct ssh_ttymodes get_ttymodes_from_conf(Seat *seat, Conf *conf)
{
    struct ssh_ttymodes modes;
    size_t i;

    static const struct mode_name_type {
        const char *mode;
        int opcode;
        enum { TYPE_CHAR, TYPE_BOOL } type;
    } modes_names_types[] = {
        #define TTYMODE_CHAR(name, val, index) { #name, val, TYPE_CHAR },
        #define TTYMODE_FLAG(name, val, field, mask) { #name, val, TYPE_BOOL },
        #include "sshttymodes.h"
        #undef TTYMODE_CHAR
        #undef TTYMODE_FLAG
    };

    memset(&modes, 0, sizeof(modes));

    for (i = 0; i < lenof(modes_names_types); i++) {
        const struct mode_name_type *mode = &modes_names_types[i];
        const char *sval = conf_get_str_str(conf, CONF_ttymodes, mode->mode);
        char *to_free = NULL;

        if (!sval)
            sval = "N";                /* just in case */

        /*
         * sval[0] can be
         *  - 'V', indicating that an explicit value follows it;
         *  - 'A', indicating that we should pass the value through from
         *    the local environment via get_ttymode; or
         *  - 'N', indicating that we should explicitly not send this
         *    mode.
         */
        if (sval[0] == 'A') {
            sval = to_free = seat_get_ttymode(seat, mode->mode);
        } else if (sval[0] == 'V') {
            sval++;                    /* skip the 'V' */
        } else {
            /* else 'N', or something from the future we don't understand */
            continue;
        }

        if (sval) {
            /*
             * Parse the string representation of the tty mode
             * into the integer value it will take on the wire.
             */
            unsigned ival = 0;

            switch (mode->type) {
              case TYPE_CHAR:
                if (*sval) {
                    char *next = NULL;
                    /* We know ctrlparse won't write to the string, so
                     * casting away const is ugly but allowable. */
                    ival = ctrlparse((char *)sval, &next);
                    if (!next)
                        ival = sval[0];
                } else {
                    ival = 255; /* special value meaning "don't set" */
                }
                break;
              case TYPE_BOOL:
                if (stricmp(sval, "yes") == 0 ||
                    stricmp(sval, "on") == 0 ||
                    stricmp(sval, "true") == 0 ||
                    stricmp(sval, "+") == 0)
                    ival = 1;      /* true */
                else if (stricmp(sval, "no") == 0 ||
                         stricmp(sval, "off") == 0 ||
                         stricmp(sval, "false") == 0 ||
                         stricmp(sval, "-") == 0)
                    ival = 0;      /* false */
                else
                    ival = (atoi(sval) != 0);
                break;
              default:
                assert(0 && "Bad mode->type");
            }

            modes.have_mode[mode->opcode] = TRUE;
            modes.mode_val[mode->opcode] = ival;
        }

        sfree(to_free);
    }

    {
        unsigned ospeed, ispeed;

        /* Unpick the terminal-speed config string. */
        ospeed = ispeed = 38400;           /* last-resort defaults */
        sscanf(conf_get_str(conf, CONF_termspeed), "%u,%u", &ospeed, &ispeed);
        /* Currently we unconditionally set these */
        modes.have_mode[TTYMODE_ISPEED] = TRUE;
        modes.mode_val[TTYMODE_ISPEED] = ispeed;
        modes.have_mode[TTYMODE_OSPEED] = TRUE;
        modes.mode_val[TTYMODE_OSPEED] = ospeed;
    }

    return modes;
}

struct ssh_ttymodes read_ttymodes_from_packet(
    BinarySource *bs, int ssh_version)
{
    struct ssh_ttymodes modes;
    memset(&modes, 0, sizeof(modes));

    while (1) {
        unsigned real_opcode, our_opcode;

        real_opcode = get_byte(bs);
        if (real_opcode == TTYMODE_END_OF_LIST)
            break;
        if (real_opcode >= 160) {
            /*
             * RFC 4254 (and the SSH 1.5 spec): "Opcodes 160 to 255
             * are not yet defined, and cause parsing to stop (they
             * should only be used after any other data)."
             *
             * My interpretation of this is that if one of these
             * opcodes appears, it's not a parse _error_, but it is
             * something that we don't know how to parse even well
             * enough to step over it to find the next opcode, so we
             * stop parsing now and assume that the rest of the string
             * is composed entirely of things we don't understand and
             * (as usual for unsupported terminal modes) silently
             * ignore.
             */
            return modes;
        }

        our_opcode = our_ttymode_opcode(real_opcode, ssh_version);
        assert(our_opcode < TTYMODE_LIMIT);
        modes.have_mode[our_opcode] = TRUE;

        if (ssh_version == 1 && real_opcode >= 1 && real_opcode <= 127)
            modes.mode_val[our_opcode] = get_byte(bs);
        else
            modes.mode_val[our_opcode] = get_uint32(bs);
    }

    return modes;
}

void write_ttymodes_to_packet(BinarySink *bs, int ssh_version,
                              struct ssh_ttymodes modes)
{
    unsigned i;

    for (i = 0; i < TTYMODE_LIMIT; i++) {
        if (modes.have_mode[i]) {
            unsigned val = modes.mode_val[i];
            unsigned opcode = real_ttymode_opcode(i, ssh_version);

            put_byte(bs, opcode);
            if (ssh_version == 1 && opcode >= 1 && opcode <= 127)
                put_byte(bs, val);
            else
                put_uint32(bs, val);
        }
    }

    put_byte(bs, TTYMODE_END_OF_LIST);
}

/* ----------------------------------------------------------------------
 * Routine for allocating a new channel ID, given a means of finding
 * the index field in a given channel structure.
 */

unsigned alloc_channel_id_general(tree234 *channels, size_t localid_offset)
{
    const unsigned CHANNEL_NUMBER_OFFSET = 256;
    search234_state ss;

    /*
     * First-fit allocation of channel numbers: we always pick the
     * lowest unused one.
     *
     * Every channel before that, and no channel after it, has an ID
     * exactly equal to its tree index plus CHANNEL_NUMBER_OFFSET. So
     * we can use the search234 system to identify the length of that
     * initial sequence, in a single log-time pass down the channels
     * tree.
     */
    search234_start(&ss, channels);
    while (ss.element) {
        unsigned localid = *(unsigned *)((char *)ss.element + localid_offset);
        if (localid == ss.index + CHANNEL_NUMBER_OFFSET)
            search234_step(&ss, +1);
        else
            search234_step(&ss, -1);
    }

    /*
     * Now ss.index gives exactly the number of channels in that
     * initial sequence. So adding CHANNEL_NUMBER_OFFSET to it must
     * give precisely the lowest unused channel number.
     */
    return ss.index + CHANNEL_NUMBER_OFFSET;
}

/* ----------------------------------------------------------------------
 * Functions for handling the comma-separated strings used to store
 * lists of protocol identifiers in SSH-2.
 */

int first_in_commasep_string(char const *needle, char const *haystack,
                             int haylen)
{
    int needlen;
    if (!needle || !haystack)          /* protect against null pointers */
        return 0;
    needlen = strlen(needle);

    if (haylen >= needlen &&       /* haystack is long enough */
        !memcmp(needle, haystack, needlen) &&   /* initial match */
        (haylen == needlen || haystack[needlen] == ',')
        /* either , or EOS follows */
        )
        return 1;
    return 0;
}

int in_commasep_string(char const *needle, char const *haystack, int haylen)
{
    char *p;

    if (!needle || !haystack)          /* protect against null pointers */
        return FALSE;
    /*
     * Is it at the start of the string?
     */
    if (first_in_commasep_string(needle, haystack, haylen))
        return TRUE;
    /*
     * If not, search for the next comma and resume after that.
     * If no comma found, terminate.
     */
    p = memchr(haystack, ',', haylen);
    if (!p)
        return FALSE;
    /* + 1 to skip over comma */
    return in_commasep_string(needle, p + 1, haylen - (p + 1 - haystack));
}

void add_to_commasep(strbuf *buf, const char *data)
{
    if (buf->len > 0)
        put_byte(buf, ',');
    put_data(buf, data, strlen(data));
}

int get_commasep_word(ptrlen *list, ptrlen *word)
{
    const char *comma;

    /*
     * Discard empty list elements, should there be any, because we
     * never want to return one as if it was a real string. (This
     * introduces a mild tolerance of badly formatted data in lists we
     * receive, but I think that's acceptable.)
     */
    while (list->len > 0 && *(const char *)list->ptr == ',') {
        list->ptr = (const char *)list->ptr + 1;
        list->len--;
    }

    if (!list->len)
        return FALSE;

    comma = memchr(list->ptr, ',', list->len);
    if (!comma) {
        *word = *list;
        list->len = 0;
    } else {
        size_t wordlen = comma - (const char *)list->ptr;
        word->ptr = list->ptr;
        word->len = wordlen;
        list->ptr = (const char *)list->ptr + wordlen + 1;
        list->len -= wordlen + 1;
    }
    return TRUE;
}

/* ----------------------------------------------------------------------
 * Functions for translating SSH packet type codes into their symbolic
 * string names.
 */

#define TRANSLATE_UNIVERSAL(y, name, value)      \
    if (type == value) return #name;
#define TRANSLATE_KEX(y, name, value, ctx) \
    if (type == value && pkt_kctx == ctx) return #name;
#define TRANSLATE_AUTH(y, name, value, ctx) \
    if (type == value && pkt_actx == ctx) return #name;

const char *ssh1_pkt_type(int type)
{
    SSH1_MESSAGE_TYPES(TRANSLATE_UNIVERSAL, y);
    return "unknown";
}
const char *ssh2_pkt_type(Pkt_KCtx pkt_kctx, Pkt_ACtx pkt_actx, int type)
{
    SSH2_MESSAGE_TYPES(TRANSLATE_UNIVERSAL, TRANSLATE_KEX, TRANSLATE_AUTH, y);
    return "unknown";
}

#undef TRANSLATE_UNIVERSAL
#undef TRANSLATE_KEX
#undef TRANSLATE_AUTH

/* ----------------------------------------------------------------------
 * Common helper function for clients and implementations of
 * PacketProtocolLayer.
 */

void ssh_ppl_replace(PacketProtocolLayer *old, PacketProtocolLayer *new)
{
    new->bpp = old->bpp;
    ssh_ppl_setup_queues(new, old->in_pq, old->out_pq);
    new->selfptr = old->selfptr;
    new->user_input = old->user_input;
    new->seat = old->seat;
    new->ssh = old->ssh;

    *new->selfptr = new;
    ssh_ppl_free(old);

    /* The new layer might need to be the first one that sends a
     * packet, so trigger a call to its main coroutine immediately. If
     * it doesn't need to go first, the worst that will do is return
     * straight away. */
    queue_idempotent_callback(&new->ic_process_queue);
}

void ssh_ppl_free(PacketProtocolLayer *ppl)
{
    delete_callbacks_for_context(ppl);
    ppl->vt->free(ppl);
}

static void ssh_ppl_ic_process_queue_callback(void *context)
{
    PacketProtocolLayer *ppl = (PacketProtocolLayer *)context;
    ssh_ppl_process_queue(ppl);
}

void ssh_ppl_setup_queues(PacketProtocolLayer *ppl,
                          PktInQueue *inq, PktOutQueue *outq)
{
    ppl->in_pq = inq;
    ppl->out_pq = outq;
    ppl->in_pq->pqb.ic = &ppl->ic_process_queue;
    ppl->ic_process_queue.fn = ssh_ppl_ic_process_queue_callback;
    ppl->ic_process_queue.ctx = ppl;

    /* If there's already something on the input queue, it will want
     * handling immediately. */
    if (pq_peek(ppl->in_pq))
        queue_idempotent_callback(&ppl->ic_process_queue);
}

void ssh_ppl_user_output_string_and_free(PacketProtocolLayer *ppl, char *text)
{
    /* Messages sent via this function are from the SSH layer, not
     * from the server-side process, so they always have the stderr
     * flag set. */
    seat_stderr(ppl->seat, text, strlen(text));
    sfree(text);
}

/* ----------------------------------------------------------------------
 * Common helper functions for clients and implementations of
 * BinaryPacketProtocol.
 */

static void ssh_bpp_input_raw_data_callback(void *context)
{
    BinaryPacketProtocol *bpp = (BinaryPacketProtocol *)context;
    ssh_bpp_handle_input(bpp);
}

static void ssh_bpp_output_packet_callback(void *context)
{
    BinaryPacketProtocol *bpp = (BinaryPacketProtocol *)context;
    ssh_bpp_handle_output(bpp);
}

void ssh_bpp_common_setup(BinaryPacketProtocol *bpp)
{
    pq_in_init(&bpp->in_pq);
    pq_out_init(&bpp->out_pq);
    bpp->input_eof = FALSE;
    bpp->ic_in_raw.fn = ssh_bpp_input_raw_data_callback;
    bpp->ic_in_raw.ctx = bpp;
    bpp->ic_out_pq.fn = ssh_bpp_output_packet_callback;
    bpp->ic_out_pq.ctx = bpp;
    bpp->out_pq.pqb.ic = &bpp->ic_out_pq;
}

void ssh_bpp_free(BinaryPacketProtocol *bpp)
{
    delete_callbacks_for_context(bpp);
    bpp->vt->free(bpp);
}

void ssh2_bpp_queue_disconnect(BinaryPacketProtocol *bpp,
                               const char *msg, int category)
{
    PktOut *pkt = ssh_bpp_new_pktout(bpp, SSH2_MSG_DISCONNECT);
    put_uint32(pkt, category);
    put_stringz(pkt, msg);
    put_stringz(pkt, "en");            /* language tag */
    pq_push(&bpp->out_pq, pkt);
}

#define BITMAP_UNIVERSAL(y, name, value)         \
    | (value >= y && value < y+32 ? 1UL << (value-y) : 0)
#define BITMAP_CONDITIONAL(y, name, value, ctx) \
    BITMAP_UNIVERSAL(y, name, value)
#define SSH2_BITMAP_WORD(y) \
    (0 SSH2_MESSAGE_TYPES(BITMAP_UNIVERSAL, BITMAP_CONDITIONAL, \
                          BITMAP_CONDITIONAL, (32*y)))

int ssh2_bpp_check_unimplemented(BinaryPacketProtocol *bpp, PktIn *pktin)
{
    static const unsigned valid_bitmap[] = {
        SSH2_BITMAP_WORD(0),
        SSH2_BITMAP_WORD(1),
        SSH2_BITMAP_WORD(2),
        SSH2_BITMAP_WORD(3),
        SSH2_BITMAP_WORD(4),
        SSH2_BITMAP_WORD(5),
        SSH2_BITMAP_WORD(6),
        SSH2_BITMAP_WORD(7),
    };

    if (pktin->type < 0x100 &&
        !((valid_bitmap[pktin->type >> 5] >> (pktin->type & 0x1F)) & 1)) {
        PktOut *pkt = ssh_bpp_new_pktout(bpp, SSH2_MSG_UNIMPLEMENTED);
        put_uint32(pkt, pktin->sequence);
        pq_push(&bpp->out_pq, pkt);
        return TRUE;
    }

    return FALSE;
}

#undef BITMAP_UNIVERSAL
#undef BITMAP_CONDITIONAL
#undef SSH1_BITMAP_WORD

/* ----------------------------------------------------------------------
 * Function to check a host key against any manually configured in Conf.
 */

int verify_ssh_manual_host_key(
    Conf *conf, const char *fingerprint, ssh_key *key)
{
    if (!conf_get_str_nthstrkey(conf, CONF_ssh_manual_hostkeys, 0))
        return -1;                     /* no manual keys configured */

    if (fingerprint) {
        /*
         * The fingerprint string we've been given will have things
         * like 'ssh-rsa 2048' at the front of it. Strip those off and
         * narrow down to just the colon-separated hex block at the
         * end of the string.
         */
        const char *p = strrchr(fingerprint, ' ');
        fingerprint = p ? p+1 : fingerprint;
        /* Quick sanity checks, including making sure it's in lowercase */
        assert(strlen(fingerprint) == 16*3 - 1);
        assert(fingerprint[2] == ':');
        assert(fingerprint[strspn(fingerprint, "0123456789abcdef:")] == 0);

        if (conf_get_str_str_opt(conf, CONF_ssh_manual_hostkeys, fingerprint))
            return 1;                  /* success */
    }

    if (key) {
        /*
         * Construct the base64-encoded public key blob and see if
         * that's listed.
         */
        strbuf *binblob;
        char *base64blob;
        int atoms, i;
        binblob = strbuf_new();
        ssh_key_public_blob(key, BinarySink_UPCAST(binblob));
        atoms = (binblob->len + 2) / 3;
        base64blob = snewn(atoms * 4 + 1, char);
        for (i = 0; i < atoms; i++)
            base64_encode_atom(binblob->u + 3*i,
                               binblob->len - 3*i, base64blob + 4*i);
        base64blob[atoms * 4] = '\0';
        strbuf_free(binblob);
        if (conf_get_str_str_opt(conf, CONF_ssh_manual_hostkeys, base64blob)) {
            sfree(base64blob);
            return 1;                  /* success */
        }
        sfree(base64blob);
    }

    return 0;
}

/* ----------------------------------------------------------------------
 * Common functions shared between SSH-1 layers.
 */

int ssh1_common_get_specials(
    PacketProtocolLayer *ppl, add_special_fn_t add_special, void *ctx)
{
    /*
     * Don't bother offering IGNORE if we've decided the remote
     * won't cope with it, since we wouldn't bother sending it if
     * asked anyway.
     */
    if (!(ppl->remote_bugs & BUG_CHOKES_ON_SSH1_IGNORE)) {
        add_special(ctx, "IGNORE message", SS_NOP, 0);
        return TRUE;
    }

    return FALSE;
}

int ssh1_common_filter_queue(PacketProtocolLayer *ppl)
{
    PktIn *pktin;
    ptrlen msg;

    while ((pktin = pq_peek(ppl->in_pq)) != NULL) {
        switch (pktin->type) {
          case SSH1_MSG_DISCONNECT:
            msg = get_string(pktin);
            ssh_remote_error(ppl->ssh,
                             "Remote side sent disconnect message:\n\"%.*s\"",
                             PTRLEN_PRINTF(msg));
            pq_pop(ppl->in_pq);
            return TRUE;               /* indicate that we've been freed */

          case SSH1_MSG_DEBUG:
            msg = get_string(pktin);
            ppl_logevent(("Remote debug message: %.*s", PTRLEN_PRINTF(msg)));
            pq_pop(ppl->in_pq);
            break;

          case SSH1_MSG_IGNORE:
            /* Do nothing, because we're ignoring it! Duhh. */
            pq_pop(ppl->in_pq);
            break;

          default:
            return FALSE;
        }
    }

    return FALSE;
}

void ssh1_compute_session_id(
    unsigned char *session_id, const unsigned char *cookie,
    struct RSAKey *hostkey, struct RSAKey *servkey)
{
    struct MD5Context md5c;
    int i;

    MD5Init(&md5c);
    for (i = (bignum_bitcount(hostkey->modulus) + 7) / 8; i-- ;)
        put_byte(&md5c, bignum_byte(hostkey->modulus, i));
    for (i = (bignum_bitcount(servkey->modulus) + 7) / 8; i-- ;)
        put_byte(&md5c, bignum_byte(servkey->modulus, i));
    put_data(&md5c, cookie, 8);
    MD5Final(session_id, &md5c);
}

/* ----------------------------------------------------------------------
 * Other miscellaneous utility functions.
 */

void free_rportfwd(struct ssh_rportfwd *rpf)
{
    if (rpf) {
        sfree(rpf->log_description);
        sfree(rpf->shost);
        sfree(rpf->dhost);
        sfree(rpf);
    }
}
