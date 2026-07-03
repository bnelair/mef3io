// mef3io — nanobind bindings. Fleshed out per phase; P0 exposes just enough
// (crc + version) to prove the toolchain end to end.
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <span>
#include <optional>
#include <vector>

#include "mef3io/crc.hpp"
#include "mef3io/crypto.hpp"
#include "mef3io/metadata.hpp"
#include "mef3io/reader.hpp"
#include "mef3io/red.hpp"
#include "mef3io/session.hpp"
#include "mef3io/session_writer.hpp"
#include "mef3io/types.hpp"
#include "mef3io/writer.hpp"

#include <cmath>
#include <filesystem>

namespace nb = nanobind;

namespace {
// Move a vector<T> onto the heap and hand numpy ownership via a capsule.
template <typename T>
nb::ndarray<nb::numpy, T> vec_to_numpy(std::vector<T>&& v) {
  auto* heap = new std::vector<T>(std::move(v));
  nb::capsule owner(heap, [](void* p) noexcept { delete static_cast<std::vector<T>*>(p); });
  size_t n = heap->size();
  return nb::ndarray<nb::numpy, T>(heap->data(), {n}, owner);
}
}  // namespace

namespace {
std::span<const mef3io::ui1> as_span(nb::bytes b) {
  return {reinterpret_cast<const mef3io::ui1*>(b.c_str()), b.size()};
}
nb::bytes to_bytes(std::span<const mef3io::ui1> s) {
  return nb::bytes(reinterpret_cast<const char*>(s.data()), s.size());
}
}  // namespace

NB_MODULE(_mef3io, m) {
  m.doc() = "mef3io C++ backend (nanobind extension)";
  m.attr("__mef_version_major__") = mef3io::fmt::MEF_VERSION_MAJOR;
  m.attr("__mef_version_minor__") = mef3io::fmt::MEF_VERSION_MINOR;

  // Exposed for parity tests against the Python oracles.
  m.def(
      "crc32", [](nb::bytes data) { return mef3io::crc::calculate(as_span(data)); },
      nb::arg("data"), "CRC-32 (Koopman) over the given bytes.");

  m.def(
      "sha256",
      [](nb::bytes data) {
        auto h = mef3io::crypto::sha256(as_span(data));
        return to_bytes(h);
      },
      nb::arg("data"));

  m.def(
      "aes128_ecb_encrypt",
      [](nb::bytes data, nb::bytes key) {
        return to_bytes(mef3io::crypto::aes128_ecb_encrypt(as_span(data), as_span(key)));
      },
      nb::arg("data"), nb::arg("key"));

  m.def(
      "aes128_ecb_decrypt",
      [](nb::bytes data, nb::bytes key) {
        return to_bytes(mef3io::crypto::aes128_ecb_decrypt(as_span(data), as_span(key)));
      },
      nb::arg("data"), nb::arg("key"));

  m.def("extract_password_bytes", [](const std::string& p) {
    auto b = mef3io::crypto::extract_password_bytes(p);
    return to_bytes(b);
  });

  m.def("validate_password", [](const std::string& password, nb::bytes l1, nb::bytes l2) {
    auto keys = mef3io::crypto::validate_password(password, as_span(l1), as_span(l2));
    return keys.access_level;
  });

  // Parse a .tmet image -> dict of the fields the reader cares about. Used by
  // the P1 test to validate struct layout + decryption against golden files.
  m.def(
      "parse_tmet",
      [](nb::bytes tmet, const std::string& password) {
        auto md = mef3io::load_time_series_metadata(as_span(tmet), password);
        nb::dict d;
        d["channel_name"] = md.universal_header.channel_name;
        d["session_name"] = md.universal_header.session_name;
        d["segment_number"] = md.universal_header.segment_number;
        d["start_time"] = md.universal_header.start_time;
        d["end_time"] = md.universal_header.end_time;
        d["access_level"] = md.access_level;
        d["section3_available"] = md.section3_available;
        d["sampling_frequency"] = md.section2.sampling_frequency;
        d["units_conversion_factor"] = md.section2.units_conversion_factor;
        d["units_description"] = md.section2.units_description;
        d["number_of_samples"] = md.section2.number_of_samples;
        d["number_of_blocks"] = md.section2.number_of_blocks;
        d["start_sample"] = md.section2.start_sample;
        d["recording_time_offset"] = md.section3.recording_time_offset;
        d["gmt_offset"] = md.section3.gmt_offset;
        return d;
      },
      nb::arg("tmet"), nb::arg("password") = "");

  // Round-trip the universal header (parse -> serialize) and return the 1024
  // re-serialized bytes; the P1 test checks they equal the original, which
  // validates the serialize-side field offsets ahead of the write phase.
  m.def("roundtrip_universal_header", [](nb::bytes file_bytes) {
    auto uh = mef3io::fmt::UniversalHeader::parse(as_span(file_bytes));
    std::vector<mef3io::ui1> out(mef3io::fmt::UNIVERSAL_HEADER_BYTES);
    uh.serialize(out);
    return to_bytes(out);
  });

  // RED encode/decode round-trip helper (P4 validation). Takes int32 samples,
  // returns the decoded int32 samples after a full encode->decode cycle.
  m.def("red_roundtrip", [](nb::ndarray<const mef3io::si4, nb::ndim<1>, nb::c_contig> samples,
                            bool discontinuity) {
    std::span<const mef3io::si4> s(samples.data(), samples.size());
    mef3io::si4 mn = 0, mx = 0;
    auto block = mef3io::red::encode_block(s, 12345, discontinuity, mn, mx);
    auto dec = mef3io::red::decode_block(block);
    nb::dict d;
    d["samples"] = vec_to_numpy(std::move(dec.samples));
    d["min"] = mn;
    d["max"] = mx;
    d["block_bytes"] = static_cast<mef3io::si8>(block.size());
    d["discontinuity"] = dec.discontinuity;
    return d;
  });

  // Write one single-segment time-series channel (P4 validation helper).
  // Splits `samples` into RED blocks of `block_len`; block 0 is marked as a
  // discontinuity (meflib convention for a segment's first block).
  m.def(
      "write_test_segment",
      [](const std::string& mefd_path, const std::string& channel, const std::string& session_name,
         nb::ndarray<const mef3io::si4, nb::ndim<1>, nb::c_contig> samples, mef3io::si8 start_uutc,
         double fs, double ufact, const std::string& units, mef3io::si8 block_len,
         const std::string& password1, const std::string& password2) {
        namespace fs_ns = std::filesystem;
        std::string seg = channel + "-000000";
        auto seg_dir = fs_ns::path(mefd_path) / (channel + ".timd") / (seg + ".segd");
        fs_ns::create_directories(seg_dir);

        std::span<const mef3io::si4> all(samples.data(), samples.size());
        std::vector<mef3io::BlockSpec> blocks;
        mef3io::si8 n = static_cast<mef3io::si8>(all.size());
        for (mef3io::si8 s = 0; s < n; s += block_len) {
          mef3io::si8 len = std::min(block_len, n - s);
          mef3io::BlockSpec b;
          b.samples.assign(all.begin() + s, all.begin() + s + len);
          b.start_sample = s;
          b.start_uutc = start_uutc + static_cast<mef3io::si8>(std::llround(s / fs * 1e6));
          b.discontinuity = (s == 0);
          blocks.push_back(std::move(b));
        }

        mef3io::SegmentSpec spec;
        spec.session_name = session_name;
        spec.channel_name = channel;
        spec.segment_number = 0;
        spec.sampling_frequency = fs;
        spec.units_conversion_factor = ufact;
        spec.units_description = units;
        spec.gmt_offset = -21600;
        spec.password_1 = password1;
        spec.password_2 = password2;
        return mef3io::write_time_series_segment(seg_dir.string(), spec, blocks);
      },
      nb::arg("mefd_path"), nb::arg("channel"), nb::arg("session_name"), nb::arg("samples"),
      nb::arg("start_uutc"), nb::arg("fs"), nb::arg("ufact"), nb::arg("units") = "uV",
      nb::arg("block_len") = 2560, nb::arg("password1") = "", nb::arg("password2") = "");

  // --- Session (P2 low-level reads) ---
  nb::class_<mef3io::Session>(m, "Session")
      .def(nb::init<const std::string&, std::string>(), nb::arg("path"), nb::arg("password") = "")
      .def_prop_ro("channels", &mef3io::Session::channels)
      .def("channel_info",
           [](mef3io::Session& s, const std::string& name) {
             const auto& ci = s.channel_info(name);
             nb::dict d;
             d["name"] = ci.name;
             d["sampling_frequency"] = ci.sampling_frequency;
             d["units_conversion_factor"] = ci.units_conversion_factor;
             d["units_description"] = ci.units_description;
             d["start_time"] = ci.start_time;
             d["end_time"] = ci.end_time;
             d["number_of_samples"] = ci.number_of_samples;
             d["recording_time_offset"] = ci.recording_time_offset;
             d["n_segments"] = ci.n_segments;
             return d;
           })
      .def(
          "read_runs",
          [](mef3io::Session& s, const std::string& channel, nb::object t0, nb::object t1) {
            std::optional<mef3io::si8> a, b;
            if (!t0.is_none()) a = nb::cast<mef3io::si8>(t0);
            if (!t1.is_none()) b = nb::cast<mef3io::si8>(t1);
            auto runs = s.read_runs(channel, a, b);
            nb::list out;
            for (auto& r : runs) {
              nb::dict d;
              d["start_uutc"] = r.start_uutc;
              d["start_sample"] = r.start_sample;
              d["samples"] = vec_to_numpy(std::move(r.samples));
              out.append(d);
            }
            return out;
          },
          nb::arg("channel"), nb::arg("t0") = nb::none(), nb::arg("t1") = nb::none());

  // --- SessionWriter (P5 high-level write) ---
  auto summary_dict = [](const mef3io::WriteSummary& s) {
    nb::dict d;
    d["samples_written"] = s.samples_written;
    d["blocks"] = s.blocks;
    d["gaps_skipped"] = s.gaps_skipped;
    d["segment"] = s.segment;
    return d;
  };
  nb::class_<mef3io::SessionWriter>(m, "SessionWriter")
      .def(nb::init<const std::string&, bool, std::string, std::string>(), nb::arg("path"),
           nb::arg("overwrite") = false, nb::arg("password1") = "", nb::arg("password2") = "")
      .def("set_block_length", &mef3io::SessionWriter::set_block_length)
      .def("set_units", &mef3io::SessionWriter::set_units)
      .def("set_threads", &mef3io::SessionWriter::set_threads)
      .def(
          "write_float",
          [summary_dict](mef3io::SessionWriter& w, const std::string& ch,
                         nb::ndarray<const mef3io::sf8, nb::ndim<1>, nb::c_contig> data,
                         mef3io::si8 start, double fs, int precision, bool new_segment) {
            std::span<const mef3io::sf8> s(data.data(), data.size());
            mef3io::WriteSummary r;
            {
              nb::gil_scoped_release rel;
              r = w.write_float(ch, s, start, fs, precision, new_segment);
            }
            return summary_dict(r);
          },
          nb::arg("channel"), nb::arg("data"), nb::arg("start_uutc"), nb::arg("fs"),
          nb::arg("precision") = -1, nb::arg("new_segment") = false)
      .def(
          "write_int32",
          [summary_dict](mef3io::SessionWriter& w, const std::string& ch,
                         nb::ndarray<const mef3io::si4, nb::ndim<1>, nb::c_contig> data,
                         double ufact, mef3io::si8 start, double fs, nb::object valid,
                         bool new_segment) {
            std::span<const mef3io::si4> s(data.data(), data.size());
            std::vector<mef3io::ui1> vbuf;
            std::span<const mef3io::ui1> vspan;
            if (!valid.is_none()) {
              auto v = nb::cast<nb::ndarray<const mef3io::ui1, nb::ndim<1>, nb::c_contig>>(valid);
              vspan = std::span<const mef3io::ui1>(v.data(), v.size());
            }
            mef3io::WriteSummary r = w.write_int32(ch, s, ufact, start, fs, vspan, new_segment);
            return summary_dict(r);
          },
          nb::arg("channel"), nb::arg("data"), nb::arg("ufact"), nb::arg("start_uutc"),
          nb::arg("fs"), nb::arg("valid") = nb::none(), nb::arg("new_segment") = false)
      .def(
          "write_records",
          [](mef3io::SessionWriter& w, nb::object channel, nb::list records) {
            std::optional<std::string> ch;
            if (!channel.is_none()) ch = nb::cast<std::string>(channel);
            std::vector<mef3io::Record> recs;
            for (auto item : records) {
              nb::dict d = nb::cast<nb::dict>(item);
              mef3io::Record r;
              r.type = nb::cast<std::string>(d["type"]);
              r.time = nb::cast<mef3io::si8>(d["time"]);
              if (d.contains("text")) r.text = nb::cast<std::string>(d["text"]);
              if (d.contains("duration")) r.duration = nb::cast<mef3io::si8>(d["duration"]);
              recs.push_back(std::move(r));
            }
            w.write_records(ch, recs);
          },
          nb::arg("channel").none(), nb::arg("records"));

  m.def("infer_precision", [](nb::ndarray<const mef3io::sf8, nb::ndim<1>, nb::c_contig> d) {
    return mef3io::infer_precision(std::span<const mef3io::sf8>(d.data(), d.size()));
  });

  // --- Reader (P3 high-level API) ---
  auto opt_time = [](nb::object o) -> std::optional<mef3io::si8> {
    if (o.is_none()) return std::nullopt;
    return nb::cast<mef3io::si8>(o);
  };

  nb::class_<mef3io::Reader>(m, "Reader")
      .def(nb::init<const std::string&, std::string, int>(), nb::arg("path"),
           nb::arg("password") = "", nb::arg("n_threads") = 0)
      .def("set_threads", &mef3io::Reader::set_threads)
      .def_prop_ro("channels", &mef3io::Reader::channels)
      .def("info",
           [](mef3io::Reader& r, const std::string& ch) {
             const auto& ci = r.info(ch);
             nb::dict d;
             d["name"] = ci.name;
             d["sampling_frequency"] = ci.sampling_frequency;
             d["units_conversion_factor"] = ci.units_conversion_factor;
             d["units_description"] = ci.units_description;
             d["start_time"] = ci.start_time;
             d["end_time"] = ci.end_time;
             d["number_of_samples"] = ci.number_of_samples;
             d["recording_time_offset"] = ci.recording_time_offset;
             d["n_segments"] = ci.n_segments;
             return d;
           })
      .def(
          "read",
          [opt_time](mef3io::Reader& r, const std::string& ch, nb::object t0, nb::object t1,
                     int n_threads) {
            std::vector<mef3io::sf8> v;
            {
              nb::gil_scoped_release rel;
              v = r.read(ch, opt_time(t0), opt_time(t1), n_threads);
            }
            return vec_to_numpy(std::move(v));
          },
          nb::arg("channel"), nb::arg("t0") = nb::none(), nb::arg("t1") = nb::none(),
          nb::arg("n_threads") = mef3io::Reader::kUseDefaultThreads)
      .def(
          "read_raw",
          [opt_time](mef3io::Reader& r, const std::string& ch, nb::object t0, nb::object t1,
                     int n_threads) {
            mef3io::RawData d;
            {
              nb::gil_scoped_release rel;
              d = r.read_raw(ch, opt_time(t0), opt_time(t1), n_threads);
            }
            nb::dict out;
            out["start_uutc"] = d.start_uutc;
            out["sampling_frequency"] = d.sampling_frequency;
            out["units_conversion_factor"] = d.units_conversion_factor;
            out["samples"] = vec_to_numpy(std::move(d.samples));
            out["valid"] = vec_to_numpy(std::move(d.valid));
            return out;
          },
          nb::arg("channel"), nb::arg("t0") = nb::none(), nb::arg("t1") = nb::none(),
          nb::arg("n_threads") = mef3io::Reader::kUseDefaultThreads)
      .def("segments",
           [](mef3io::Reader& r, const std::string& ch) {
             auto segs = r.segments(ch);
             nb::list out;
             for (const auto& s : segs) {
               nb::dict d;
               d["segment"] = s.segment_number;
               d["path"] = s.path;
               d["start_time"] = s.start_time;
               d["end_time"] = s.end_time;
               d["start_sample"] = s.start_sample;
               d["number_of_samples"] = s.number_of_samples;
               d["number_of_blocks"] = s.number_of_blocks;
               out.append(d);
             }
             return out;
           })
      .def("toc",
           [](mef3io::Reader& r, const std::string& ch) {
             auto entries = r.toc(ch);
             nb::list out;
             for (const auto& e : entries) {
               nb::dict d;
               d["start_uutc"] = e.start_uutc;
               d["start_sample"] = e.start_sample;
               d["number_of_samples"] = e.number_of_samples;
               d["maximum_sample_value"] = e.maximum_sample_value;
               d["minimum_sample_value"] = e.minimum_sample_value;
               d["discontinuity"] = e.discontinuity;
               out.append(d);
             }
             return out;
           })
      .def(
          "records",
          [](mef3io::Reader& r, nb::object channel) {
            std::optional<std::string> ch;
            if (!channel.is_none()) ch = nb::cast<std::string>(channel);
            auto recs = r.records(ch);
            nb::list out;
            for (const auto& rec : recs) {
              nb::dict d;
              d["type"] = rec.type;
              d["time"] = rec.time;
              if (rec.text) d["text"] = *rec.text;
              if (rec.duration) d["duration"] = *rec.duration;
              if (rec.earliest_onset) d["earliest_onset"] = *rec.earliest_onset;
              if (rec.latest_offset) d["latest_offset"] = *rec.latest_offset;
              out.append(d);
            }
            return out;
          },
          nb::arg("channel") = nb::none());
}
