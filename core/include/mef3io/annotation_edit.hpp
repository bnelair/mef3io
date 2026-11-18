// mef3io — remove (and so change) stored annotations.
//
// `write_records` / Writer.write_annotations only ever APPEND. This is the one
// other way annotations change: remove_annotations() takes the records out of
// a record file pair (`.rdat` + `.ridx`) by CONTENT and, in the same call,
// may add others, so "change a note" is "remove it and add the corrected
// one" in ONE rewrite. A removed record is gone: there is no undo and no
// history (the old files are kept only until the new ones are in place).
//
// What it touches: ONLY the record files of the levels named (session,
// channel, segment). Signal files and metadata are never opened for writing.
// Every record that stays is carried over BYTE FOR BYTE (header, body, CRC,
// encryption level, time, order); the two universal headers are made right
// as for a file written fresh (count, span, entry size, CRCs, provenance
// "records-edit").
//
// Crash safety (for_agents/notes/records-and-time.md, "Removing annotations"):
//   1. everything is checked, selected and built IN MEMORY first: nothing is
//      written when any selector fails (all or nothing);
//   2. the new files are written BESIDE the old ones (`<file>.<id>.new`),
//      flushed, then read back and verified against the intended result;
//   3. the operation marker `.mef3io-operation` is written (it carries the
//      plan: it IS the journal) and flushed with its directory;
//   4. each old file is renamed to `<file>.<id>.old`, then its new file into
//      its place; 5. the `.old` files are removed; 6. the marker is removed.
// A crash before step 3 leaves the session as it was (plus unused `.new`
// files). A crash from step 3 to 6 leaves a marker: reads still work but the
// open warns (SessionOperationWarning), writes refuse, the validator reports
// `records.interrupted-edit`, and recover_session(apply) FINISHES the edit
// (or, when a new file is not intact, rolls it back): never a mixed pair left
// silently. The next remove_annotations does the same before its own work.
//
// Python + C++ only (no C ABI, no MATLAB).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mef3io/control.hpp"
#include "mef3io/records.hpp"
#include "mef3io/transform.hpp"  // CopyError: the coded refusals
#include "mef3io/types.hpp"

namespace mef3io {

/// Names the stored record(s) to remove, by CONTENT as `Reader.records()`
/// reads them back: where (level, channel, segment), when (`time`), what kind
/// (`type`) and every body field. A selector never names a position.
///
/// Every body field the record's type carries must be given when the stored
/// record has a value for it (a field omitted, or given as nullopt, matches
/// only a record whose field has no value); fields are compared exactly
/// (numbers by value, text byte for byte). A selector with a field its type
/// does not carry is refused, so is one that omits a field a candidate has.
struct RecordSelector {
  RecordLevel level = RecordLevel::Session;
  std::string channel;  ///< channel and segment level
  int segment = -1;     ///< segment level: the segment number
  std::string type;     ///< "Note", "EDFA", ...
  si8 time = 0;         ///< absolute uUTC, as read back
  /// Body fields, names and values as `record_fields()` / `Reader.records()`
  /// give them.
  std::vector<RecordField> fields;
  /// How many stored records this selector must match. Fewer or more is a
  /// refusal and NOTHING is removed. Identical records (the same note stored
  /// twice) are the ordinary way to match more than one.
  si8 expect = 1;
  /// Remove every copy, however many (at least one must exist): `expect` is
  /// then not checked.
  bool all_copies = false;
};

/// The selector that names exactly `record` (a record read back from
/// Session::read_records), one copy.
RecordSelector selector_for(const Record& record);

struct RemoveAnnotationsOptions {
  /// The session's passwords, both or neither, exactly as for Writer: the
  /// session decides whether they are needed (require_record_write_access).
  std::string password_1, password_2;
  /// Select, build and verify everything in memory, report, write nothing.
  bool dry_run = false;
  /// Finish an earlier interrupted edit even if the program that started it still seems to be
  /// running on this machine (or its marker is of a newer layout): the owner check is skipped
  /// and recovery settles as for a dead owner. Only when you are sure no program is editing
  /// this session; the report says it was used.
  bool assume_owner_gone = false;
  /// For tests only: stop at this step as a crash would (nothing cleaned up)
  /// by throwing IoError. -1 (default): never. See annotation_edit.cpp for
  /// the step list; a value past the last step does nothing.
  int crash_at = -1;
  /// For tests only: fail at this step with an ordinary error, which the
  /// operation rolls back itself. -1 (default): never.
  int fail_at = -1;
};

/// One record file pair this call rewrote (or would, in a dry run).
struct RecordFileEdit {
  RecordLevel level = RecordLevel::Session;
  std::string channel;  ///< "" at session level
  int segment = -1;
  std::string file;     ///< session-relative `.rdat` path, '/'-separated
  si8 records_before = 0;
  si8 removed = 0;
  si8 added = 0;
  si8 records_after = 0;
  /// The last record went: the pair was removed (nothing is left to describe;
  /// a later write_annotations creates a fresh one).
  bool pair_removed = false;
};

struct RemoveAnnotationsReport {
  std::vector<RecordFileEdit> files;
  si8 removed = 0;
  si8 added = 0;
  bool dry_run = false;
  /// An earlier edit this session had been interrupted in (its id), finished
  /// (or rolled back) before this call's own work.
  std::string resumed;
  std::vector<std::string> notes;
};

/// Remove the records named by `remove` and add `add`, in ONE crash-safe
/// rewrite of the record files of the levels involved.
///
/// `add` records are written like write_annotations writes them (type,
/// time, text, duration; level-2 encrypted in a protected session) and placed
/// at the level in their own `level` / `channel` / `segment` members (default
/// session level); that level's file must already exist (use write_annotations
/// to create one). They are stored after the kept records, in time order.
///
/// Refusals are CopyError-coded exceptions of their usual type (code list in
/// docs/python.md, "remove_annotations"): tar archives (`tar-read-only`), a record file that
/// cannot be read in full (`record-damaged`, `record-crc`,
/// `record-index-mismatch`, `record-locked`), a wrong or missing password
/// (`password`), selectors that match nothing (`no-match`), too few or too
/// many (`ambiguous`, `count-mismatch`) or one record twice
/// (`selectors-overlap`), a session being written or mid-operation
/// (`being-written`, `mid-operation`), records changed while the call ran
/// (`records-changed`), an unknown recording-time offset (`offset-unknown`).
/// Nothing is changed by any refusal.
///
/// @throws Cancelled  `ctl.cancel` was set before the new files were in place:
///         the session is as it was. After that point cancel is ignored (the
///         swap is a few renames).
RemoveAnnotationsReport remove_annotations(const std::string& path,
                                           const std::vector<RecordSelector>& remove,
                                           const std::vector<Record>& add = {},
                                           const RemoveAnnotationsOptions& opts = {},
                                           const Control& ctl = {});

/// An annotation edit this session was interrupted in, and what finishing it
/// does. `applied` is false for a description (nothing was changed).
struct InterruptedRecordEdit {
  std::string id;                  ///< the edit's id (names its temporary files)
  std::vector<std::string> files;  ///< session-relative record files it concerns
  /// "finish" (every new file is intact: the swap is completed), "roll back"
  /// (a new file is damaged or missing and the old ones are still there: the
  /// removal was NOT done) or "settle" (the marker's plan cannot be read: the
  /// files alone decide, see `reason`).
  std::string action;
  /// Why recovery goes back, or what it found when it settles from the files.
  std::string reason;
  /// The program that placed the marker is a live process on this machine (not the
  /// caller): the edit is RUNNING, not interrupted, and recovery refuses
  /// (`edit-in-progress`) until it ends. False when its owner is dead, on another machine
  /// or unknown (an unreadable plan).
  bool running = false;
  /// `assume_owner_gone` was needed and used (the report records it).
  bool owner_override = false;
  bool applied = false;
};

/// The interrupted edit the session carries, if any (reads only).
std::optional<InterruptedRecordEdit> interrupted_record_edit(const std::string& path,
                                                             bool assume_owner_gone = false);

/// Finish (or roll back) the interrupted edit, then remove its marker.
/// Idempotent: a crash in here is finished by the next call. `crash_at` /
/// `fail_at` are the test hooks of RemoveAnnotationsOptions. nullopt when the
/// session carries no such edit.
std::optional<InterruptedRecordEdit> finish_interrupted_record_edit(const std::string& path,
                                                                    int crash_at = -1,
                                                                    int fail_at = -1,
                                                                    bool assume_owner_gone = false);

/// Whether the session carries the marker of an interrupted annotation edit
/// (a directory session; never throws). While it does, a level's record files
/// may be half swapped, so EVERY reader of records (Session::read_records, so
/// Reader, MultiReader, the extension, the C ABI and MATLAB; inspect_session,
/// which reports it as a warning instead) refuses or says so, and signal
/// reads keep working.
bool has_interrupted_edit(const std::string& path);

/// Throws the coded refusal "edit-interrupted" (WriteConflictError) when
/// has_interrupted_edit(path). Called by Session::read_records.
void require_no_interrupted_edit(const std::string& path);

/// Temporary files an edit left (`<record file>.<id>.new` / `.old`, a marker
/// `.part`) in a session that carries no edit marker: harmless, never read.
/// Returns their session-relative paths; removes them when `apply`. With a
/// marker present nothing is touched.
std::vector<std::string> record_edit_leftovers(const std::string& path, bool apply);

/// TEST SEAM (never needed by a caller; it only OBSERVES, it cannot change what an
/// edit does, so it is compiled into every build): every durable step an edit takes, in
/// order, as `(kind, absolute path)`: "fsync-file", "flush-dir", "rename"
/// (path = "from|to"), "remove", "marker-durable", "direction-durable",
/// "marker-removed" and "point" (a crash/failure point). The observer runs
/// on the editing thread, may act (a test appends to a record file from it)
/// and must not call back into the library for the same session. Pass an
/// empty function to stop.
namespace testing {
using EditObserver = std::function<void(const std::string& kind, const std::string& path)>;
void set_edit_observer(EditObserver observer);
/// Called by a writer at a point a test may want to act at ("before-append",
/// "after-append" around the record append of SessionWriter::write_records).
void emit_write_event(const std::string& kind, const std::string& path);
#ifdef MEF3IO_TEST_SEAMS
/// Make the edit skip its folder flushes and file flushes (the negative
/// control of the durability-order test: the test must then fail). ONLY in a
/// build configured with -DMEF3IO_TEST_SEAMS=ON (the C++ test build); a release
/// library has no such function.
void set_edit_skip_flush(bool skip);
#endif
}  // namespace testing

}  // namespace mef3io
