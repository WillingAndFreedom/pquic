#include "picoquic_internal.h"
#include "plugin.h"
#include "../helpers.h"
#include "bpf.h"

typedef enum {
    picoquic_newreno_alg_slow_start = 0,
    picoquic_newreno_alg_recovery,
    picoquic_newreno_alg_congestion_avoidance
} picoquic_newreno_alg_state_t;

typedef struct st_picoquic_newreno_state_t {
    picoquic_newreno_alg_state_t alg_state;
    uint64_t residual_ack;
    uint64_t ssthresh;
    uint64_t recovery_start;
} picoquic_newreno_state_t;

/* Ugly, but with the uBPF VM, not possible to do better...
 * The recovery state last 1 RTT, during which parameters will be frozen
 */
static void picoquic_newreno_enter_recovery(picoquic_path_t* path_x,
    picoquic_newreno_state_t* nr_state,
    uint64_t current_time)
{
    nr_state->ssthresh = path_x->cwin / 2;
    if (nr_state->ssthresh < PICOQUIC_CWIN_MINIMUM) {
        nr_state->ssthresh = PICOQUIC_CWIN_MINIMUM;
    }
    path_x->cwin = nr_state->ssthresh;

    nr_state->recovery_start = current_time;

    nr_state->residual_ack = 0;

    nr_state->alg_state = picoquic_newreno_alg_recovery;
}

/**
 * See PROTOOP_PARAM_PROCESS_FRAME
 */
protoop_arg_t process_ack_frame(picoquic_cnx_t *cnx)
{ 
    ack_frame_t *frame = (ack_frame_t *) cnx->protoop_inputv[0];
    uint64_t current_time = (uint64_t) cnx->protoop_inputv[1];
    int epoch = (int) cnx->protoop_inputv[2];

    picoquic_path_t* path_x = cnx->path[0];
    picoquic_packet_context_enum pc = helper_context_from_epoch(epoch);
    uint8_t first_byte = (frame->is_ack_ecn) ? picoquic_frame_type_ack_ecn : picoquic_frame_type_ack;

    picoquic_newreno_state_t *nrs = path_x->congestion_alg_state;
    bpf_data *bpfd = get_bpf_data(cnx);

    if (epoch == 1) {
        protoop_arg_t args[1];
        args[0] = first_byte;
        if (frame->is_ack_ecn) {
            helper_protoop_printf(cnx, "Ack-ECN frame (0x%x) not expected in 0-RTT packet", args, 1);
        } else {
            helper_protoop_printf(cnx, "Ack-ECN frame (0x%x) not expected in 0-RTT packet", args, 1);
        }
        helper_connection_error(cnx, PICOQUIC_TRANSPORT_PROTOCOL_VIOLATION, first_byte);
        return 1;
    } else if (frame->largest_acknowledged >= path_x->pkt_ctx[pc].send_sequence) {
        helper_connection_error(cnx, PICOQUIC_TRANSPORT_PROTOCOL_VIOLATION, first_byte);
        return 1;
    } else {
        if (frame->is_ack_ecn) {
            cnx->ecn_ect0_total_remote = frame->ecnx3[0];
            cnx->ecn_ect1_total_remote = frame->ecnx3[1];
            cnx->ecn_ce_total_remote = frame->ecnx3[2];
        }

        /* Attempt to update the RTT */
        picoquic_packet_t* top_packet = helper_update_rtt(cnx, frame->largest_acknowledged, current_time, frame->ack_delay, pc, path_x);

        uint64_t range = frame->first_ack_block;
        uint64_t block_to_block;

        range ++;

        if (frame->largest_acknowledged + 1 < range) {
            protoop_arg_t args[2];
            args[0] = frame->largest_acknowledged;
            args[1] = range;
            helper_protoop_printf(cnx, "ack range error: largest=%" PRIx64 ", range=%" PRIx64, args, 2);
            helper_connection_error(cnx, PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR, first_byte);
            return 1;
        }

        if (helper_process_ack_range(cnx, pc, frame->largest_acknowledged, range, &top_packet, current_time) != 0) {
            return 1;
        }

        if (range > 0) {
            helper_check_spurious_retransmission(cnx, frame->largest_acknowledged + 1 - range, frame->largest_acknowledged, current_time, pc, path_x);
        }

        uint64_t largest = frame->largest_acknowledged;

        for (int i = 0; i < frame->ack_block_count; i++) {
            /* Skip the gap */
            block_to_block = frame->ack_blocks[i].gap;

            block_to_block += 1; /* add 1, since zero is ruled out by varint, see spec. */
            block_to_block += range;

            if (largest < block_to_block) {
                protoop_arg_t args[3];
                args[0] = largest;
                args[1] = range;
                args[2] = block_to_block - range;
                helper_protoop_printf(cnx, "ack gap error: largest=%" PRIx64 ", range=%" PRIx64 ", gap=%" PRIu64, args, 3);
                helper_connection_error(cnx, PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR, first_byte);
                return 1;
            }

            largest -= block_to_block;
            range = frame->ack_blocks[i].additional_ack_block;
            range ++;
            if (largest + 1 < range) {
                protoop_arg_t args[2];
                args[0] = largest;
                args[1] = range;
                helper_protoop_printf(cnx, "ack range error: largest=%" PRIx64 ", range=%" PRIx64, args, 2);
                helper_connection_error(cnx, PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR, first_byte);
                return 1;
            }

            if (helper_process_ack_range(cnx, pc, largest, range, &top_packet, current_time) != 0) {
                return 1;
            }

            if (range > 0) {
                helper_check_spurious_retransmission(cnx, largest + 1 - range, largest, current_time, pc, path_x);
            }
        }
    }

    if (nrs->alg_state != picoquic_newreno_alg_recovery && bpfd->ecn_ect_ce_remote_pkts > bpfd->ecn_ack_ce_counter) {
        picoquic_newreno_enter_recovery(path_x, nrs, current_time);
    }

    bpfd->ecn_ack_ce_counter = bpfd->ecn_ect_ce_remote_pkts;

    return 0;
}