#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <arpa/inet.h>
#include <zmq.h>
#include <alsa/asoundlib.h>

#define ZMQ_LEN 2048
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
	
	while (1)
	{
		int r = zmq_recv(zmq_ptt_sub, (uint8_t*)pmt_buff, sizeof(pmt_buff), ZMQ_DONTWAIT);
		if (r > 0)
		{
			if (memcmp(pmt_buff, sot_pmt, 6) == 0)
			{
				fprintf(stderr, "PTT pressed\n");
				
				// switch PCM devices
				snd_pcm_drop(bsb_rx);		// stop RX immediately
				snd_pcm_prepare(bsb_rx);    // reset RX device for next use
				snd_pcm_drop(bsb_tx);       // stop device
				snd_pcm_prepare(bsb_tx);    // reset device
				
				radio_state = STATE_TX;
				
				// make sure the TX starts with new baseband data (discard queued baseband)
				while (zmq_recv(zmq_sub, tx_buff, sizeof(tx_buff), ZMQ_DONTWAIT) > 0);
			}
			else if (memcmp(pmt_buff, eot_pmt, 6) == 0)
			{
				fprintf(stderr, "PTT released\n");
				
				// switch PCM devices
				snd_pcm_drop(bsb_tx);      	// stop TX immediately
				snd_pcm_prepare(bsb_tx);   	// reset TX device for next use
				snd_pcm_drop(bsb_rx);      	// stop device
				snd_pcm_prepare(bsb_rx);   	// reset device
				
				radio_state = STATE_RX;
			}
			else
				fprintf(stderr, "Unrecognized PMT\n");
		}
		
		// RX
		if (radio_state == STATE_RX)
		{
			snd_pcm_sframes_t n = snd_pcm_readi(bsb_rx, rx_buff, ZMQ_LEN/2);

			if (n == -EPIPE)
			{
				// overrun
				snd_pcm_recover(bsb_rx, n, 1);
				continue;
			}
			else if (n < 0)
			{
				// other error
				snd_pcm_recover(bsb_rx, n, 1);
				continue;
			}
			else if ((uint32_t)n < ZMQ_LEN/2)
			{
				// short read — continue
				continue;
			}
			
			zmq_send(zmq_pub, (uint8_t*)rx_buff, ZMQ_LEN*sizeof(*rx_buff), ZMQ_DONTWAIT);
		}
		
		// TX
		else if (radio_state == STATE_TX)
		{
			int r = zmq_recv(zmq_sub, (uint8_t*)tx_buff, sizeof(tx_buff), ZMQ_DONTWAIT);
			if (r > 0)
			{
				uint8_t pos = 0;
				do
				{
					snd_pcm_sframes_t written;
					do
					{
						written = snd_pcm_writei(bsb_tx, &tx_buff[ZMQ_LEN*pos], ZMQ_LEN/2);
						if (written < 0)
							written = snd_pcm_recover(bsb_tx, written, 1);
					}
					while (written < 0);
					
					r -= ZMQ_LEN*4;
					pos++;
				} 
				while (r > 0);
			}
		}
	}
	
	// shouldn't get here
	return 0;
}
