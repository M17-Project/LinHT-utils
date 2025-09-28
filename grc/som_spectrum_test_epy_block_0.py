import numpy as np
from gnuradio import gr
import os

class cyclic_file_sink(gr.sync_block):
    """
    Overwrites the first `vec_len` floats of a file with each input vector.
    Input vector length must match `vec_len`.
    """
    def __init__(self, filename="out.bin", vec_len=256):
        gr.sync_block.__init__(
            self,
            name="Cyclic File Sink",
            in_sig=[(np.float32, vec_len)],  # fixed vector of vec_len floats
            out_sig=None
        )
        self.filename = filename
        self.vec_len = vec_len

        # Create or truncate file to vec_len floats
        self.fh = open(self.filename, "w+b")
        self.fh.write(b"\x00" * (self.vec_len * 4))
        self.fh.flush()

    def work(self, input_items, output_items):
        in0 = input_items[0]  # shape: (#vectors, vec_len)

        for vec in in0:
            self.fh.seek(0)
            self.fh.write(np.asarray(vec, dtype=np.float32).tobytes())
            self.fh.flush()

        return len(in0)

    def stop(self):
        if self.fh:
            self.fh.close()
        return True
