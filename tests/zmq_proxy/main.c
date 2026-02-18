#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <arpa/inet.h>
#include <zmq.h>
#include <alsa/asoundlib.h>

#define ZMQ_LEN 2048
#define BYTES_PER_PERIOD (ZMQ_LEN * sizeof(int32_t)) // for ALSA
#define BSB_RX_DEV "hw:SX1255"
#define BSB_TX_DEV "hw:SX1255,1"
#define RX_IPC  "/tmp/bsb_rx"
#define TX_IPC  "/tmp/bsb_tx"
#define PTT_IPC "ipc:///tmp/ptt_msg"

uint32_t rate = 500000;
int32_t rx_buff[ZMQ_LEN], tx_buff[ZMQ_LEN*16];
uint8_t pmt_buff[64];
int retval;
snd_pcm_t *bsb_rx;
snd_pcm_t *bsb_tx;
snd_pcm_hw_params_t *dev_params;
void *zmq_ctx;
void *zmq_pub;
void *zmq_sub;
void *zmq_ptt_sub;

uint8_t sot_pmt[10], eot_pmt[10];

struct timeval tv_start, tv_now;
//int64_t t_sust = 4*40e3;	//default sustain time in microseconds (4 M17 frames)

typedef enum
{
	STATE_RX,
	STATE_TX
} state_t;

state_t radio_state = STATE_RX;

void exit_handler(int sig)
{
	(void)sig;
    fprintf(stderr, "\nCaught Ctrl-C (SIGINT). Cleaning up...\n");
	snd_pcm_drain(bsb_rx);
    snd_pcm_close(bsb_rx);
	snd_pcm_drain(bsb_tx); // required?
    snd_pcm_close(bsb_tx);
	zmq_unbind(zmq_pub, "ipc://" RX_IPC); // "tcp://*:17001"
	zmq_unbind(zmq_sub, "ipc://" TX_IPC); // "tcp://*:17002"
	zmq_disconnect(zmq_ptt_sub, PTT_IPC);
	zmq_ctx_destroy(&zmq_ctx);
	exit(EXIT_SUCCESS);
}

uint8_t string_to_pmt(uint8_t *pmt, const char *msg)
{
	pmt[0] = 2;									 // pmt type - zmq message
	*((uint16_t *)&pmt[1]) = htons(strlen(msg)); // length
	strcpy((char *)&pmt[3], msg);

	return 3 + strlen(msg);
}

int64_t time_diff_us(struct timeval a, struct timeval b)
{
    return (a.tv_sec - b.tv_sec)*1000000L + (a.tv_usec - b.tv_usec);
}

void tx_stop_cleanup(void)
{
	// stop/reset PCM devices
	snd_pcm_drop(bsb_tx);      	// stop TX immediately
	snd_pcm_prepare(bsb_tx);   	// reset TX device for next use
	snd_pcm_drop(bsb_rx);      	// stop device
	snd_pcm_prepare(bsb_rx);   	// reset device
	
	// switch state
	radio_state = STATE_RX;
}
void rx_stop_cleanup(void)
{
	// stop/reset PCM devices
	snd_pcm_drop(bsb_rx);		// stop RX immediately
	snd_pcm_prepare(bsb_rx);    // reset RX device for next use
	snd_pcm_drop(bsb_tx);       // stop device
	snd_pcm_prepare(bsb_tx);    // reset device
	
	// switch state
	radio_state = STATE_TX;
	
	// make sure the TX starts with new baseband data (discard queued baseband)
	while (zmq_recv(zmq_sub, tx_buff, sizeof(tx_buff), ZMQ_DONTWAIT) > 0);
}

int main(void)
{
	signal(SIGINT, exit_handler);
	
	zmq_ctx = zmq_ctx_new();
    zmq_pub = zmq_socket(zmq_ctx, ZMQ_PUB);
	zmq_sub = zmq_socket(zmq_ctx, ZMQ_SUB);
	zmq_ptt_sub = zmq_socket(zmq_ctx, ZMQ_SUB);
	zmq_setsockopt(zmq_sub, ZMQ_SUBSCRIBE, "", 0); // no filters
	zmq_setsockopt(zmq_ptt_sub, ZMQ_SUBSCRIBE, "", 0); // no filters
	
	if (zmq_bind(zmq_pub, "ipc://" RX_IPC) != 0) // "tcp://*:17001"
    {
        printf("ZeroMQ: baseband PUB binding error.\nExiting.\n");
        return -1;
    }
	
	if (zmq_bind(zmq_sub, "ipc://" TX_IPC) != 0) // "tcp://*:17002"
    {
        printf("ZeroMQ: baseband SUB binding error.\nExiting.\n");
        return -1;
    }
	
	if (zmq_connect(zmq_ptt_sub, PTT_IPC) != 0)
    {
        printf("ZeroMQ: PTT SUB connection error.\nExiting.\n");
        return -1;
    }	
	
	retval = snd_pcm_open(&bsb_rx, BSB_RX_DEV, SND_PCM_STREAM_CAPTURE, 0);
	if (retval != 0)
	{
		fprintf(stderr, "Failed to open baseband input device\n");
		return -1;
	}

	retval = snd_pcm_open(&bsb_tx, BSB_TX_DEV, SND_PCM_STREAM_PLAYBACK, 0);
	if (retval != 0)
	{
		fprintf(stderr, "Failed to open baseband output device\n");
		return -1;
	}
	
	// RX
	snd_pcm_hw_params_malloc(&dev_params);
    snd_pcm_hw_params_any(bsb_rx, dev_params);
    snd_pcm_hw_params_set_access(bsb_rx, dev_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(bsb_rx, dev_params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(bsb_rx, dev_params, 2);
    snd_pcm_hw_params_set_rate(bsb_rx, dev_params, rate, 0);
    snd_pcm_hw_params_set_period_size(bsb_rx, dev_params, ZMQ_LEN, 0);
    snd_pcm_hw_params(bsb_rx, dev_params);
    snd_pcm_hw_params_free(dev_params);
	
	// TX
	snd_pcm_hw_params_malloc(&dev_params);
    snd_pcm_hw_params_any(bsb_tx, dev_params);
    snd_pcm_hw_params_set_access(bsb_tx, dev_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(bsb_tx, dev_params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(bsb_tx, dev_params, 2);
    snd_pcm_hw_params_set_rate(bsb_tx, dev_params, rate, 0);
    snd_pcm_hw_params_set_period_size(bsb_tx, dev_params, ZMQ_LEN, 0);
    snd_pcm_hw_params(bsb_tx, dev_params);
    snd_pcm_hw_params_free(dev_params);
	
    retval = snd_pcm_prepare(bsb_rx);
	if (retval != 0)
	{
		fprintf(stderr, "Error\n");
		return -1;
	}
	
	retval = snd_pcm_prepare(bsb_tx);
	if (retval != 0)
	{
		fprintf(stderr, "Error\n");
		return -1;
	}
	
	string_to_pmt(sot_pmt, "SOT");
	string_to_pmt(eot_pmt, "EOT");
		
	fprintf(stderr, "Running...\n");

	zmq_pollitem_t zitems[] =
	{
		{ zmq_ptt_sub, 0, ZMQ_POLLIN, 0 },
		{ zmq_sub,     0, ZMQ_POLLIN, 0 },
	};

	while (1)
	{
		// handle PTT + TX baseband readiness (non-blocking)
		zmq_poll(zitems, 2, 0);   // no wait, just update revents

		if (zitems[0].revents & ZMQ_POLLIN)
		{
			zmq_recv(zmq_ptt_sub, (uint8_t*)pmt_buff, sizeof(pmt_buff), ZMQ_DONTWAIT);

			if (memcmp(pmt_buff, sot_pmt, 6) == 0)
			{
				fprintf(stderr, "PTT pressed\n");
				rx_stop_cleanup();
			}
			else if (memcmp(pmt_buff, eot_pmt, 6) == 0)
			{
				fprintf(stderr, "PTT released\n");
				gettimeofday(&tv_start, NULL);
			}
			/*else if (strncmp((char*)&pmt_buff[3], "SUST", 4) == 0)
			{
				int32_t val = atoi((char*)&pmt_buff[7]);
				t_sust = (int64_t)val * 1000;
				fprintf(stderr, "Setting sustain time to %d ms\n", val);
			}*/
			else
			{
				fprintf(stderr, "Unrecognized PMT message\n");
			}
		}

		// RX state
		if (radio_state == STATE_RX)
		{
			// wait for RX device to be ready, up to 100 ms
			int w = snd_pcm_wait(bsb_rx, 100);
			if (w < 0)
			{
				snd_pcm_recover(bsb_rx, w, 1);
				continue;
			}

			snd_pcm_sframes_t n = snd_pcm_readi(bsb_rx, rx_buff, ZMQ_LEN/2);

			if (n == -EPIPE)
			{
				snd_pcm_recover(bsb_rx, n, 1);
				continue;
			}
			else if (n < 0)
			{
				snd_pcm_recover(bsb_rx, n, 1);
				continue;
			}
			else if ((uint32_t)n < ZMQ_LEN/2)
			{
				// short read - ignore
				continue;
			}

			zmq_send(zmq_pub, (uint8_t*)rx_buff,
					 ZMQ_LEN * sizeof(*rx_buff),
					 ZMQ_DONTWAIT);
		}

		// TX state
		else if (radio_state == STATE_TX)
		{
			zmq_poll(zitems, 2, 10);   // Repoll but this time blocking to prevent busy loop

			// new baseband from UDP/ZMQ side?
			if (zitems[1].revents & ZMQ_POLLIN)
			{
				int r = zmq_recv(zmq_sub, (uint8_t*)tx_buff, sizeof(tx_buff), 0); // blocking is fine here

				if (r > 0)
				{
					// optional: sanity-check packet size
					/*if (r % BYTES_PER_PERIOD != 0)
					{
						fprintf(stderr, "TX: unexpected packet size %d bytes (not multiple of %d)\n",
								r, (int)BYTES_PER_PERIOD);
					}*/

					int bytes_left = r;
					uint8_t *p = (uint8_t*)tx_buff;

					// write all full periods contained in this ZMQ message
					while (bytes_left >= (int)BYTES_PER_PERIOD && radio_state == STATE_TX)
					{
						snd_pcm_sframes_t written;

						do
						{
							written = snd_pcm_writei(
								bsb_tx,
								(int32_t*)p,      // start of this period
								ZMQ_LEN / 2       // frames per period
							);

							if (written == -EPIPE)
							{
								// underrun
								snd_pcm_recover(bsb_tx, written, 1);
							}
							else if (written < 0)
							{
								// other error
								snd_pcm_recover(bsb_tx, written, 1);
							}
						}
						while (written < 0 && radio_state == STATE_TX);

						// ALSA should either block until this period is played,
						// or recover and retry, so when we get here this period is "consumed".
						p          += BYTES_PER_PERIOD;
						bytes_left -= BYTES_PER_PERIOD;
					}

					// any leftover bytes (less than one full period) are ignored for now;
					// if you ever see 'r' not equal to BYTES_PER_PERIOD, fix the sender.
				}
			}

			// check if the "tx sustain" time has elapsed
			/*if (tv_start.tv_sec != 0)
			{
				gettimeofday(&tv_now, NULL);
				if (time_diff_us(tv_now, tv_start) >= t_sust)
				{
					tx_stop_cleanup();
					fprintf(stderr, " TX sustain time elapsed\n");
					tv_start.tv_sec = 0;
				}
			}*/
		}
	}
	
	// shouldn't get here
	return 0;
}
