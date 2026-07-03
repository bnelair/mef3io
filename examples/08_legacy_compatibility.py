

import time

import numpy as np
import scipy.signal as signal
import matplotlib.pyplot as plt

from datetime import datetime
from tqdm import tqdm
from pathlib import Path

from mef_tools import MefReader as MefReaderLegacy
from mef_tools import MefWriter as MefWriterLegacy

from mef3io import MefReader
from mef3io import MefWriter
from mef3io import Reader

pth_leg = '/Users/mivalt.filip/Data/tmp/test_wrt_leg.mefd'
pth_new = '/Users/mivalt.filip/Data/tmp/test_wrt_new.mefd'

fs = 512
len_s = 3600*5
channels = ['A', 'B', 'C', 'D', 'E']
start = datetime(2020, 1, 1, 0, 0, 0).timestamp()
end = start + len_s

x = np.random.randn(len(channels), fs*len_s)
b1, a1 = signal.butter(3, 100, 'low', fs=fs)
b2, a2 = signal.butter(1, 100, 'high', fs=fs)

x = signal.filtfilt(b1, a1, x, axis=-1)
x = signal.filtfilt(b2, a2, x, axis=-1)

x[0, 1000:2000] = np.nan
x[1, 5000] = np.nan
# x[2, 0:10] = np.nan
# x[3, -50:] = np.nan

# ===== LEGACY WRITE WITH TIMING =====
start_time_leg = time.time()
wrt_leg = MefWriterLegacy(pth_leg, overwrite=True, password1='pwd1', password2='pwd2')
wrt_leg.mef_block_len = 1000
wrt_leg.max_nans_written = 0
wrt_leg.data_units = 'uV'

for idx, ch in tqdm(list(enumerate(channels))):
    wrt_leg.write_data(x[idx], ch, int(start * 1e6), fs, precision=3)
time_write_leg = time.time() - start_time_leg

# ===== NEW WRITE WITH TIMING =====
start_time_new = time.time()
wrt = MefWriter(pth_new, overwrite=True, password1='pwd1', password2='pwd2')
wrt.mef_block_len = 1000
wrt.max_nans_written = 0
wrt.data_units = 'uV'
for idx, ch in tqdm(list(enumerate(channels))):
    wrt.write_data(x[idx], ch, int(start * 1e6), fs, precision=3)
time_write_new = time.time() - start_time_new

file_size_leg = sum(f.stat().st_size for f in Path(pth_leg).rglob('*') if f.is_file())
file_size_new = sum(f.stat().st_size for f in Path(pth_new).rglob('*') if f.is_file())

print(f"Write Time - Legacy: {time_write_leg:.2f}s | New: {time_write_new:.2f}s")
print(f"File Size in MB - Legacy: {file_size_leg / 1e6:.2f} | New: {file_size_new / 1e6:.2f}")

# ===== READ LEGACY WRITTEN FILE WITH TIMING =====
# HEADER READING - Legacy reader
start_time = time.time()
rdr1_leg = MefReaderLegacy(pth_leg, password2='pwd2')
time_read_header_leg_on_leg = time.time() - start_time

# ACTUAL DATA READING - Legacy reader
start_time = time.time()
x1 = rdr1_leg.get_data(channels)
time_read_data_leg_on_leg = time.time() - start_time

# HEADER READING - New reader
start_time = time.time()
rdr1_new = MefReader(pth_leg, password2='pwd2')
time_read_header_new_on_leg = time.time() - start_time

# ACTUAL DATA READING - New reader
start_time = time.time()
x2 = rdr1_new.get_data(channels)
time_read_data_new_on_leg = time.time() - start_time

x1 = np.array(x1)
x2 = np.array(x2)

print('X'*20)
print("Comparison on a legacy written file using legacy and new reader")
print(f"Read Header Time - Legacy reader: {time_read_header_leg_on_leg:.2f}s | New reader: {time_read_header_new_on_leg:.2f}s")
print(f"Read Data Time - Legacy reader: {time_read_data_leg_on_leg:.2f}s | New reader: {time_read_data_new_on_leg:.2f}s")
print(f"Total Read Time - Legacy reader: {time_read_header_leg_on_leg + time_read_data_leg_on_leg:.2f}s | New reader: {time_read_header_new_on_leg + time_read_data_new_on_leg:.2f}s")
print(f"Legacy data - legacy reader: shape {x1.shape} | average values {np.nanmean(x1, axis=-1)} | number of nans {np.isnan(x1).sum()}")

print(f"New data - new reader: shape {x2.shape} | average values {np.nanmean(x2, axis=-1)} | number of nans {np.isnan(x2).sum()}")
print(f"Data equality: {np.allclose(x1, x2, equal_nan=True)}")
print(f"NaN positions match: {np.array_equal(np.isnan(x1), np.isnan(x2))}")

# ===== READ NEW WRITTEN FILE WITH TIMING =====
# HEADER READING - Legacy reader
start_time = time.time()
rdr2_leg = MefReaderLegacy(pth_new, password2='pwd2')
time_read_header_leg_on_new = time.time() - start_time

# ACTUAL DATA READING - Legacy reader
start_time = time.time()
x3 = rdr2_leg.get_data(channels)
time_read_data_leg_on_new = time.time() - start_time

# HEADER READING - New reader
start_time = time.time()
rdr2_new = MefReader(pth_new, password2='pwd2')
time_read_header_new_on_new = time.time() - start_time

# ACTUAL DATA READING - New reader
start_time = time.time()
x4 = rdr2_new.get_data(channels)
time_read_data_new_on_new = time.time() - start_time

x3 = np.array(x3)
x4 = np.array(x4)

print('X'*20)
print("Comparison on a newly written file using legacy and new reader")
print(f"Read Header Time - Legacy reader: {time_read_header_leg_on_new:.2f}s | New reader: {time_read_header_new_on_new:.2f}s")
print(f"Read Data Time - Legacy reader: {time_read_data_leg_on_new:.2f}s | New reader: {time_read_data_new_on_new:.2f}s")
print(f"Total Read Time - Legacy reader: {time_read_header_leg_on_new + time_read_data_leg_on_new:.2f}s | New reader: {time_read_header_new_on_new + time_read_data_new_on_new:.2f}s")
print(f"Legacy reader: shape {x3.shape} | average values {np.nanmean(x3, axis=-1)} | number of nans {np.isnan(x3).sum()}")
print(f"New reader: shape {x4.shape} | average values {np.nanmean(x4, axis=-1)} | number of nans {np.isnan(x4).sum()}")
print(f"Data equality: {np.allclose(x3, x4, equal_nan=True)}")
print(f"NaN positions match: {np.array_equal(np.isnan(x3), np.isnan(x4))}")

print('X'*20)
print("=== PERFORMANCE SUMMARY ===")
print(f"Write Performance - Legacy: {time_write_leg:.2f}s | New: {time_write_new:.2f}s (speedup: {time_write_leg/time_write_new:.2f}x)")
print(f"\nRead Header on Legacy Files - Legacy: {time_read_header_leg_on_leg:.2f}s | New: {time_read_header_new_on_leg:.2f}s (speedup: {time_read_header_leg_on_leg/time_read_header_new_on_leg:.2f}x)")
print(f"Read Data on Legacy Files - Legacy: {time_read_data_leg_on_leg:.2f}s | New: {time_read_data_new_on_leg:.2f}s (speedup: {time_read_data_leg_on_leg/time_read_data_new_on_leg:.2f}x)")
print(f"\nRead Header on New Files - Legacy: {time_read_header_leg_on_new:.2f}s | New: {time_read_header_new_on_new:.2f}s (speedup: {time_read_header_leg_on_new/time_read_header_new_on_new:.2f}x)")
print(f"Read Data on New Files - Legacy: {time_read_data_leg_on_new:.2f}s | New: {time_read_data_new_on_new:.2f}s (speedup: {time_read_data_leg_on_new/time_read_data_new_on_new:.2f}x)")
print('X'*20)
print("Comparison between legacy and new writer outputs")
print(f"Legacy writer vs legacy reader: {np.allclose(x1, x3, equal_nan=True)}")
print(f"New writer vs new reader: {np.allclose(x2, x4, equal_nan=True)}")
print(f"Legacy writer vs new writer (legacy reader): {np.allclose(x1, x3, equal_nan=True)}")
print(f"Legacy writer vs new writer (new reader): {np.allclose(x2, x4, equal_nan=True)}")

# ===== ENCRYPTION TESTS ON ALREADY-GENERATED FILES =====
