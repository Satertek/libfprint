#!/home/brian/git/goodix-fp-dump/.venv/bin/python
"""
Goodix 532d Fingerprint Multi-Finger Dataset Capture & Analysis Tool
-------------------------------------------------------------------
Interactive tool to capture raw 16-bit sensor frames across multiple fingers,
evaluate genuine matches (same finger) vs impostor matches (different fingers),
and test full 10-stage enrollment coverage.
"""

import os
import sys
import time
import json
import subprocess
import glob
from pathlib import Path

REPO_DIR = Path(__file__).resolve().parent.parent
CAPTURE_BIN = REPO_DIR / "build" / "examples" / "img-capture"
NBIS_BIN = REPO_DIR / "build" / "test_nbis"
RIDGE_BIN = REPO_DIR / "build" / "test_ridge_match"
CAPTURES_DIR = REPO_DIR / "captures"

ENROLL_STAGES = [
    ("enroll_idx_01", "Touch 1 of 10: Flat Center of Right Index"),
    ("enroll_idx_02", "Touch 2 of 10: Flat Center of Right Index (press slightly firmer)"),
    ("enroll_idx_03", "Touch 3 of 10: Tip of Right Index (shifted slightly forward)"),
    ("enroll_idx_04", "Touch 4 of 10: Tip of Right Index (shifted slightly forward)"),
    ("enroll_idx_05", "Touch 5 of 10: Lower Pad of Right Index (shifted slightly backward)"),
    ("enroll_idx_06", "Touch 6 of 10: Lower Pad of Right Index (shifted slightly backward)"),
    ("enroll_idx_07", "Touch 7 of 10: Left Side of Right Index"),
    ("enroll_idx_08", "Touch 8 of 10: Left Edge of Right Index"),
    ("enroll_idx_09", "Touch 9 of 10: Right Side of Right Index"),
    ("enroll_idx_10", "Touch 10 of 10: Right Edge of Right Index"),
]

VERIFY_PROBES = [
    ("probe_idx_normal", "Verification Probe 1: Right Index (Normal touch)"),
    ("probe_idx_shifted", "Verification Probe 2: Right Index (Slightly shifted touch)"),
    ("impostor_pinky", "Impostor Test 1: Right Pinky Finger (Expect REJECT)"),
    ("impostor_thumb", "Impostor Test 2: Right Thumb Finger (Expect REJECT)"),
]

GUIDED_2SAMPLE_STEPS = [
    ("right_index", 1, "Right Index Finger - Sample 1"),
    ("right_index", 2, "Right Index Finger - Sample 2"),
    ("right_pinky", 1, "Right Pinky Finger - Sample 1"),
    ("right_pinky", 2, "Right Pinky Finger - Sample 2"),
    ("right_thumb", 1, "Right Thumb - Sample 1"),
    ("right_thumb", 2, "Right Thumb - Sample 2"),
]


def check_prerequisites():
    if not CAPTURE_BIN.exists():
        print(f"[!] Error: {CAPTURE_BIN} not found. Please compile libfprint first:")
        print("    distrobox enter bench -- ninja -C build")
        sys.exit(1)
    if not NBIS_BIN.exists():
        print(f"[!] Error: {NBIS_BIN} not found. Please compile test_nbis first:")
        print("    distrobox enter bench -- bash -c 'gcc ...'")
        sys.exit(1)
    CAPTURES_DIR.mkdir(parents=True, exist_ok=True)


def capture_single(finger_id: str, sample_idx: int, description: str) -> bool:
    prefix = str(CAPTURES_DIR / f"{finger_id}_s{sample_idx}")
    target_pgm = f"{prefix}.pgm"

    print("\n" + "=" * 60)
    print(f" >>> TARGET: {description}")
    print(f" >>> Saving to prefix: {prefix}")
    print("=" * 60)
    input(f"Press Enter when ready, then place [{description}] on sensor...")

    env = os.environ.copy()
    env["GOODIX_DUMP_PREFIX"] = prefix
    env["G_MESSAGES_DEBUG"] = "none"

    cmd = [str(CAPTURE_BIN), target_pgm]

    print("Waiting for finger touch on sensor...")
    try:
        proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        # Even if return code was non-zero, check if target or raw file was created
        raw_path = Path(f"{prefix}_raw16.pgm")
        proc64 = Path(f"{prefix}_proc64.pgm")
        proc192 = Path(f"{prefix}_proc192.pgm")

        if not (raw_path.exists() or Path(target_pgm).exists() or proc64.exists()):
            print(f"[!] Capture failed (code {proc.returncode}):")
            print(proc.stderr.strip() or proc.stdout.strip())
            return False

        print(f"[✓] Successfully captured: {description}!")

        if proc64.exists():
            print(f"    Frame: Native 64x80 clean ridge frame ({proc64.name})")
        elif proc192.exists():
            print(f"    Frame: 192x240 frame ({proc192.name})")

        print("Please lift your finger from sensor.")
        time.sleep(1.2)
        return True

    except KeyboardInterrupt:
        print("\nCapture cancelled by user.")
        return False


def run_10_stage_enroll_and_verify():
    print("\n" + "#" * 70)
    print("      10-STAGE ENROLLMENT & MULTI-PROBE VERIFICATION TEST")
    print("#" * 70)
    print("This mode simulates libfprint's 10-stage enrollment covering the")
    print("entire finger (center, tip, sides, base) to create a full gallery.")
    print("Then it tests whether same-finger probes match (>=24) and different")
    print("fingers (Pinky, Thumb) reject (<24).\n")

    input("Press Enter to begin the 10-stage enrollment...")

    def get_sample_path(tag: str, sample_idx: int = 1) -> str:
        p64 = CAPTURES_DIR / f"{tag}_s{sample_idx}_proc64.pgm"
        if p64.exists():
            return str(p64)
        p192 = CAPTURES_DIR / f"{tag}_s{sample_idx}_proc192.pgm"
        if p192.exists():
            return str(p192)
        return str(CAPTURES_DIR / f"{tag}_s{sample_idx}.pgm")

    gallery_files = []
    for tag, desc in ENROLL_STAGES:
        success = capture_single(tag, 1, desc)
        if not success:
            retry = input("Stage failed. Retry this stage? [Y/n]: ").strip().lower()
            if retry != 'n':
                capture_single(tag, 1, desc)
        spath = get_sample_path(tag, 1)
        if Path(spath).exists():
            gallery_files.append(spath)

    print(f"\n[✓] Enrollment Complete: Collected {len(gallery_files)} of 10 gallery stages.")

    print("\n" + "=" * 70)
    print("             NOW TESTING VERIFICATION PROBES")
    print("=" * 70)

    matcher_bin = str(RIDGE_BIN) if RIDGE_BIN.exists() else str(NBIS_BIN)
    threshold = 36 if RIDGE_BIN.exists() else 24

    probe_results = []
    for tag, desc in VERIFY_PROBES:
        capture_single(tag, 1, desc)
        probe_path = get_sample_path(tag, 1)
        if Path(probe_path).exists() and gallery_files:
            cmd = [matcher_bin, probe_path] + gallery_files
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

            score = 0
            best_stage = 0
            for line in res.stdout.splitlines():
                if "2D Ridge Match Score:" in line:
                    try:
                        score = int(line.split("2D Ridge Match Score:")[1].split("(")[0].strip())
                        best_stage = int(line.split("Best Stage:")[1].split(",")[0].strip())
                    except Exception:
                        pass
                elif "Bozorth match score between" in line:
                    score = int(line.split(":")[-1].strip())
                elif "Best Stage:" in line and best_stage == 0:
                    try:
                        best_stage = int(line.split("Best Stage:")[1].split(",")[0].strip())
                    except Exception:
                        pass
            probe_results.append((tag, desc, score, best_stage))

    print("\n" + "#" * 70)
    print("                     FINAL VERIFICATION REPORT")
    print("#" * 70)
    print(f"{'Probe Test':<45} {'Max Score':<12} {'Best Stage':<12} {'Verdict':<15}")
    print("-" * 84)

    for tag, desc, score, stage in probe_results:
        is_genuine = "idx" in tag
        if is_genuine:
            status = "✓ MATCH (PASS)" if score >= threshold else "✗ NO MATCH (FAIL)"
        else:
            status = "✓ REJECTED (PASS)" if score < threshold else "✗ FALSE MATCH (FAIL)"

        short_desc = desc.split(":")[0] + ": " + desc.split(":")[1].split("(")[0].strip()
        print(f"{short_desc:<45} {score:<12} {stage:<12} {status:<15}")

    print("#" * 70)


def run_2sample_guided():
    print("\n--- Starting Quick 2-Sample Guided Capture ---")
    for finger_id, sample_idx, desc in GUIDED_2SAMPLE_STEPS:
        capture_single(finger_id, sample_idx, desc)
    analyze_dataset()


def run_custom_capture():
    print("\n--- Custom Finger Capture ---")
    finger = input("Enter finger identifier (e.g. right_middle): ").strip()
    if not finger:
        return
    sample = input("Enter sample index [default: 1]: ").strip() or "1"
    try:
        sample_idx = int(sample)
    except ValueError:
        sample_idx = 1
    capture_single(finger, sample_idx, f"{finger.replace('_', ' ').title()} - Sample {sample_idx}")


def analyze_dataset():
    print("\n" + "=" * 70)
    print("                    DATASET MATCH MATRIX ANALYSIS")
    print("=" * 70)

    samples = sorted(glob.glob(str(CAPTURES_DIR / "*_proc64.pgm")))
    if not samples:
        samples = sorted(glob.glob(str(CAPTURES_DIR / "*_proc192.pgm")))
    if not samples:
        samples = sorted(glob.glob(str(CAPTURES_DIR / "*.pgm")))
    if not samples:
        print("[!] No captured images found in captures/ directory.")
        return

    sample_names = [Path(s).stem.replace("_proc64", "").replace("_proc192", "") for s in samples]
    n = len(sample_names)

    matcher_bin = str(RIDGE_BIN) if RIDGE_BIN.exists() else str(NBIS_BIN)
    threshold = 36 if RIDGE_BIN.exists() else 24

    print(f"Found {n} captured finger samples using matcher: {Path(matcher_bin).name} (Threshold = {threshold})\n")

    matrix = [[0] * n for _ in range(n)]

    for i in range(n):
        for j in range(i, n):
            if i == j:
                matrix[i][j] = 100
                continue
            res = subprocess.run([matcher_bin, samples[i], samples[j]],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            score = 0
            for line in res.stdout.splitlines():
                if "2D Ridge Match Score:" in line:
                    try:
                        score = int(line.split("2D Ridge Match Score:")[1].split("(")[0].strip())
                    except Exception:
                        pass
                elif "score =" in line:
                    try:
                        score = int(line.split("score =")[1].strip())
                    except Exception:
                        pass
                elif "Bozorth match score between" in line:
                    score = int(line.split(":")[-1].strip())
            matrix[i][j] = score
            matrix[j][i] = score

    header = f"{'Sample':<25}" + "".join([f"{name[:7]:>8}" for name in sample_names])
    print("\n" + header)
    print("-" * len(header))
    for i in range(n):
        row = f"{sample_names[i]:<25}"
        for j in range(n):
            score = matrix[i][j]
            row += f"{score:>8}"
        print(row)

    print("\nSummary of Verification Performance:")
    print("-------------------------------------")
    same_scores = []
    diff_scores = []

    for i in range(n):
        f1 = sample_names[i].split("_s")[0]
        for j in range(i + 1, n):
            f2 = sample_names[j].split("_s")[0]
            score = matrix[i][j]
            if f1 == f2:
                same_scores.append((sample_names[i], sample_names[j], score))
            else:
                diff_scores.append((sample_names[i], sample_names[j], score))

    if same_scores:
        print(f"Genuine Comparisons (SAME finger, expected >= {threshold}):")
        for s1, s2, sc in same_scores:
            status = "✓ PASS (MATCH)" if sc >= threshold else "✗ FAIL (NO MATCH)"
            print(f"  [{s1}] vs [{s2}]: Score = {sc:<4} -> {status}")
    else:
        print("  (Need at least 2 samples of the same finger to evaluate genuine matches)")

    if diff_scores:
        print("\nImpostor Comparisons (DIFFERENT fingers, expected < 24):")
        for s1, s2, sc in diff_scores:
            status = "✓ PASS (REJECTED)" if sc < 24 else "✗ FAIL (FALSE MATCH)"
            print(f"  [{s1}] vs [{s2}]: Score = {sc:<4} -> {status}")

    print("=" * 70)


def main():
    check_prerequisites()
    while True:
        print("\n==================================================")
        print(" Goodix 532d Fingerprint Multi-Finger Capture Tool")
        print("==================================================")
        print(" 1) Run 10-Stage Enrollment + Verification Test (Index)")
        print(" 2) Run Quick 2-Sample Guided Capture (Index, Pinky, Thumb)")
        print(" 3) Capture a Specific Finger Sample")
        print(" 4) Analyze Captured Dataset (Cross-Match Matrix)")
        print(" 5) Delete all captures")
        print(" q) Exit")
        choice = input("\nSelect option [1-5, q]: ").strip().lower()

        if choice == '1':
            run_10_stage_enroll_and_verify()
        elif choice == '2':
            run_2sample_guided()
        elif choice == '3':
            run_custom_capture()
        elif choice == '4':
            analyze_dataset()
        elif choice == '5':
            confirm = input("Are you sure you want to delete all files in captures/? [y/N]: ").strip().lower()
            if confirm == 'y':
                for f in glob.glob(str(CAPTURES_DIR / "*.pgm")):
                    os.remove(f)
                print("[✓] Cleared captures directory.")
        elif choice in ('q', 'exit', 'quit'):
            print("Exiting.")
            break
        else:
            print("Invalid option. Please choose 1-5 or q.")


if __name__ == "__main__":
    main()
