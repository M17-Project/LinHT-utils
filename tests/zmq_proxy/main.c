#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <zmq.h>
#include <alsa/asoundlib.h>

#define ZMQ_LEN 2048
#define BSB_DEV "hw:SX1255"
#define RX_IPC  "/tmp/bsb_rx"
#define TX_IPC  "/tmp/bsb_tx"

uint32_t rate = 500000;
int32_t rx_buff[ZMQ_LEN], tx_buff[ZMQ_LEN];
int retval;
snd_pcm_t *bsb_rx;
snd_pcm_t *bsb_tx;
snd_pcm_hw_params_t *dev_params;
void *zmq_ctx;
void *zmq_pub;
void *zmq_sub;

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
	zmq_ctx_destroy(&zmq_ctx);
	exit(EXIT_SUCCESS);
}

int main(void)
{
	signal(SIGINT, exit_handler);
	
	zmq_ctx = zmq_ctx_new();
    zmq_pub = zmq_socket(zmq_ctx, ZMQ_PUB);
	zmq_sub = zmq_socket(zmq_ctx, ZMQ_SUB);
	
	if (zmq_bind(zmq_pub, "ipc://" RX_IPC) != 0) // "tcp://*:17001"
    {
        printf("ZeroMQ: PUB binding error.\nExiting.\n");
        return -1;
    }
	
	if (zmq_bind(zmq_sub, "ipc://" TX_IPC) != 0) // "tcp://*:17002"
    {
        printf("ZeroMQ: SUB binding error.\nExiting.\n");
        return -1;
    }	
	
	retval = snd_pcm_open(&bsb_rx, BSB_DEV, SND_PCM_STREAM_CAPTURE, 0);
	if (retval != 0)
	{
		fprintf(stderr, "Failed to open baseband input device\n");
		return -1;
	}

	retval = snd_pcm_open(&bsb_tx, BSB_DEV, SND_PCM_STREAM_PLAYBACK, 0);
	if (retval != 0)
	{
		fprintf(stderr, "Failed to open baseband output device\n");
		return -1;
	}
	
	snd_pcm_hw_params_malloc(&dev_params);
    snd_pcm_hw_params_any(bsb_rx, dev_params);
    snd_pcm_hw_params_set_access(bsb_rx, dev_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(bsb_rx, dev_params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(bsb_rx, dev_params, 2);
    snd_pcm_hw_params_set_rate(bsb_rx, dev_params, rate, 0);
    snd_pcm_hw_params_set_period_size(bsb_rx, dev_params, ZMQ_LEN, 0);
    snd_pcm_hw_params(bsb_rx, dev_params);
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
	
	fprintf(stderr, "Running...\n");
	
	while (1)
	{
		// RX
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
	
		//TX
		if (zmq_recv(zmq_sub, (uint8_t*)tx_buff, ZMQ_LEN*sizeof(*rx_buff), ZMQ_DONTWAIT) == ZMQ_LEN)
			snd_pcm_writei(bsb_tx, tx_buff, ZMQ_LEN/2);
	}
	
	// shouldn't get here
	return 0;
}
