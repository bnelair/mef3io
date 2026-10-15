/*
 * red_blocks.c — a tiny harness around the VENDORED meflib (NOT mef3io) used to
 * produce and check RED blocks whose statistics tables are encrypted.
 *
 * Why it exists (#80): pymef never encrypts RED blocks (RED_allocate_processing
 * _struct leaves directives.encryption_level at RED_ENCRYPTION_LEVEL_DEFAULT =
 * NO_ENCRYPTION, meflib.h:1082 / meflib.c:6486, and pymef3_file.c never changes
 * it), so the only way to get blocks encrypted "the meflib way" is to drive
 * meflib's RED_encode ourselves. The fixture it produces is the interop oracle
 * for mef3io's 16-byte statistics-encryption rule.
 *
 * Built and run by tests/make_meflib_encrypted_fixture.py; never by the test
 * suite (the committed fixture is what the tests read).
 *
 *   red_blocks encrypt <seg .tdat> <seg .tidx> <seg .tmet> <password> <level 1|2>
 *       Decode every block of the .tdat with meflib RED_decode and re-encode it
 *       with meflib RED_encode at the given encryption level, in place. Block
 *       sizes cannot change (same samples, same lossless coder; checked). Sets
 *       the matching RED_block_flags bit in each .tidx entry, then re-seals the
 *       .tdat/.tidx universal-header body and header CRCs with meflib's CRC.
 *
 *   red_blocks decode <seg .tdat> <seg .tmet> <password> <out.i32>
 *       Decode every block with meflib RED_decode (decrypting as meflib does)
 *       and write the samples as little-endian int32.
 *
 * `password` must grant LEVEL 2 access: meflib's process_password_data puts a
 * level-1-only password's key in level_2_encryption_key (meflib.c:5079), so a
 * level-1 block could not be handled with it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meflib.h"

extern MEF_GLOBALS *MEF_globals;  /* defined in meflib.c, not declared in meflib.h */

static ui1 *slurp(const char *path, si8 *size_out) {
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
	fseek(f, 0, SEEK_END);
	si8 n = ftell(f);
	fseek(f, 0, SEEK_SET);
	ui1 *buf = (ui1 *) malloc((size_t) n);
	if (fread(buf, 1, (size_t) n, f) != (size_t) n) { fprintf(stderr, "short read %s\n", path); exit(2); }
	fclose(f);
	*size_out = n;
	return buf;
}

static void spit(const char *path, ui1 *buf, si8 n) {
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(buf, 1, (size_t) n, f) != (size_t) n) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
	fclose(f);
}

/* Re-seal a MEF file's universal header: body CRC over [1024, EOF), then the
 * header CRC over [4, 1024) (meflib write_MEF_file order). */
static void reseal(ui1 *buf, si8 n) {
	UNIVERSAL_HEADER *uh = (UNIVERSAL_HEADER *) buf;
	uh->body_CRC = CRC_calculate(buf + UNIVERSAL_HEADER_BYTES, n - UNIVERSAL_HEADER_BYTES);
	uh->header_CRC = CRC_calculate(buf + CRC_BYTES, UNIVERSAL_HEADER_BYTES - CRC_BYTES);
}

static PASSWORD_DATA *keys_for(const char *tmet_path, si1 *password) {
	si8 n;
	ui1 *tmet = slurp(tmet_path, &n);
	PASSWORD_DATA *pwd = process_password_data(password, NULL, NULL, (UNIVERSAL_HEADER *) tmet);
	free(tmet);
	if (pwd->access_level < LEVEL_2_ACCESS) { fprintf(stderr, "password does not grant level 2\n"); exit(2); }
	return pwd;
}

/* RED_decode returns silently on a bad block CRC; make that an error here. */
static void require_crc(RED_BLOCK_HEADER *bh, const char *what) {
	if (CRC_validate((ui1 *) bh + CRC_BYTES, bh->block_bytes - CRC_BYTES, bh->block_CRC) != MEF_TRUE) {
		fprintf(stderr, "%s: RED block CRC mismatch\n", what); exit(1);
	}
}

static RED_PROCESSING_STRUCT *decoder(ui4 max_samples, PASSWORD_DATA *pwd) {
	return RED_allocate_processing_struct(0, 0, max_samples, RED_MAX_DIFFERENCE_BYTES(max_samples), 0, 0, pwd);
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: see header comment\n"); return 2; }
	initialize_meflib();
	/* Leave every stored time exactly as it is on disk. */
	MEF_globals->recording_time_offset_mode = RTO_IGNORE;
	MEF_globals->CRC_mode = CRC_VALIDATE | CRC_CALCULATE_ON_OUTPUT;

	if (strcmp(argv[1], "encrypt") == 0 && argc == 7) {
		si8 tdat_n, tidx_n;
		ui1 *tdat = slurp(argv[2], &tdat_n);
		ui1 *tidx = slurp(argv[3], &tidx_n);
		PASSWORD_DATA *pwd = keys_for(argv[4], argv[5]);
		si1 level = (si1) atoi(argv[6]);
		if (level != LEVEL_1_ENCRYPTION && level != LEVEL_2_ENCRYPTION) { fprintf(stderr, "level must be 1 or 2\n"); return 2; }
		ui1 mask = level == LEVEL_1_ENCRYPTION ? RED_LEVEL_1_ENCRYPTION_MASK : RED_LEVEL_2_ENCRYPTION_MASK;

		si8 n_entries = (tidx_n - UNIVERSAL_HEADER_BYTES) / TIME_SERIES_INDEX_BYTES;
		for (si8 e = 0; e < n_entries; ++e) {
			TIME_SERIES_INDEX *tsi = (TIME_SERIES_INDEX *) (tidx + UNIVERSAL_HEADER_BYTES + e * TIME_SERIES_INDEX_BYTES);
			RED_BLOCK_HEADER *orig = (RED_BLOCK_HEADER *) (tdat + tsi->file_offset);
			if (orig->flags & (RED_LEVEL_1_ENCRYPTION_MASK | RED_LEVEL_2_ENCRYPTION_MASK)) { fprintf(stderr, "block %lld already encrypted\n", (long long) e); return 1; }
			require_crc(orig, "encrypt input");
			ui4 ns = orig->number_of_samples, bb = orig->block_bytes;

			/* meflib decode of a copy (RED_decode works in place on the header). */
			ui1 *copy = (ui1 *) malloc(bb);
			memcpy(copy, orig, bb);
			RED_PROCESSING_STRUCT *dec = decoder(ns, pwd);
			dec->block_header = (RED_BLOCK_HEADER *) copy;
			RED_decode(dec);

			/* meflib encode at the requested level. */
			RED_PROCESSING_STRUCT *enc = RED_allocate_processing_struct(0, RED_MAX_COMPRESSED_BYTES(ns, 1), 0, RED_MAX_DIFFERENCE_BYTES(ns), 0, 0, pwd);
			enc->original_ptr = enc->original_data = dec->decompressed_data;
			enc->compression.mode = RED_LOSSLESS_COMPRESSION;
			enc->directives.encryption_level = level;
			enc->directives.discontinuity = (orig->flags & RED_DISCONTINUITY_MASK) ? MEF_TRUE : MEF_FALSE;
			enc->block_header->number_of_samples = ns;
			enc->block_header->start_time = orig->start_time;
			RED_encode(enc);
			RED_BLOCK_HEADER *nb = enc->block_header;
			if (nb->block_bytes != bb) { fprintf(stderr, "block %lld changed size %u -> %u\n", (long long) e, bb, nb->block_bytes); return 1; }
			require_crc(nb, "encrypt output");
			if (!(nb->flags & mask)) { fprintf(stderr, "meflib did not encrypt block %lld\n", (long long) e); return 1; }
			memcpy(orig, nb, bb);
			tsi->RED_block_flags = nb->flags;

			free(copy);
			/* enc borrowed dec's sample buffer: detach it so it is freed once. */
			enc->original_ptr = enc->original_data = NULL;
			RED_free_processing_struct(enc);
			RED_free_processing_struct(dec);
		}
		reseal(tdat, tdat_n);
		reseal(tidx, tidx_n);
		spit(argv[2], tdat, tdat_n);
		spit(argv[3], tidx, tidx_n);
		printf("encrypted %lld blocks at level %d\n", (long long) n_entries, level);
		return 0;
	}

	if (strcmp(argv[1], "decode") == 0 && argc == 6) {
		si8 tdat_n;
		ui1 *tdat = slurp(argv[2], &tdat_n);
		PASSWORD_DATA *pwd = keys_for(argv[3], argv[4]);
		FILE *out = fopen(argv[5], "wb");
		si8 off = UNIVERSAL_HEADER_BYTES, blocks = 0;
		while (off < tdat_n) {
			RED_BLOCK_HEADER *bh = (RED_BLOCK_HEADER *) (tdat + off);
			ui4 ns = bh->number_of_samples, bb = bh->block_bytes;
			require_crc(bh, "decode input");
			RED_PROCESSING_STRUCT *dec = decoder(ns, pwd);
			dec->block_header = bh;
			RED_decode(dec);
			if (bh->flags & (RED_LEVEL_1_ENCRYPTION_MASK | RED_LEVEL_2_ENCRYPTION_MASK)) {
				/* RED_decode clears the flags when it decrypts; still set = it refused. */
				fprintf(stderr, "meflib could not decode block at %lld\n", (long long) off); return 1;
			}
			fwrite(dec->decompressed_data, sizeof(si4), ns, out);
			RED_free_processing_struct(dec);
			off += bb;
			++blocks;
		}
		fclose(out);
		printf("decoded %lld blocks\n", (long long) blocks);
		return 0;
	}

	fprintf(stderr, "usage: see header comment\n");
	return 2;
}
