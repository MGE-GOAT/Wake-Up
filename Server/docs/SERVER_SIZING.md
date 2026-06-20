# Noban — Server Hardware & Internet Estimates (200 users)

Hardware specs, internet requirements, performance under load, and hosting cost estimates for
running Noban at **200 users**. Options ordered budget-friendly → best.

---

## Cost drivers

Two components scale with usage; the rest is standard and cheap:

- **GPU** — runs the voice AI (speech-to-text + intent).
- **Internet upload bandwidth** — carries the video calls (the SFU relays each call's media in
  and out, so upload is the binding constraint).

---

## 1. Internet requirements

| Spec | Value | Note |
|---|---|---|
| Type | **Symmetric** (upload = download) | Asymmetric/home lines bottleneck on upload. |
| Speed | **Target 1 Gbps** (min. 100 Mbps symmetric) | At 30 concurrent calls ≈ 70 Mbps peak; 1 Gbps ≈ 7% load. |
| IP | **Static, public** | — |
| Tier | **Business + SLA** | — |

**Bandwidth math:** ~1.5 Mbps per concurrent call, each direction. Designed peak 30 concurrent
calls = ~45 Mbps + voice/clip/overhead ≈ **~60–70 Mbps each way**.

---

## 2. Server hardware (budget-friendly → best)

Identical software on all tiers; the difference is headroom (concurrent calls + voice-command
throughput under burst).

| | **Starter** | **Mid** | **High** |
|---|---|---|---|
| **Capacity** | 200 users | 200 users + headroom to ~300 | 200 → 500+, redundancy |
| **One-time cost** | **~$1,300–1,900** | **~$3,200–4,200** | **~$5,500–8,000** |
| **GPU** | Used RTX 3090 24 GB | RTX 4090 24 GB | 2× RTX 4090 24 GB |
| **CPU / RAM** | Ryzen 7 8-core / 32 GB | Ryzen 9 12-core / 64 GB | Ryzen 9 16-core / 128 GB |
| **Storage** | 1 TB NVMe + backup | 2× 2 TB NVMe (mirrored) | RAID1 + separate backup |
| **NIC** | 1 GbE | 2.5 GbE | 10 GbE |
| **UPS** | included | included | included |

- 24 GB GPU memory fits both AI models with room to batch several commands per pass (drives the
  voice-command numbers in §3).
- Hardware is one-time; only the internet line is recurring.
- GPU memory and internet upload are the spend that matters; CPU/RAM/storage have wide margin
  on every tier.

---

## 3. Performance under load (worst case)

**Calls + app actions — flat under load.** At 30 concurrent calls the line sits at ~7% of
1 Gbps (no congestion): call delay stays ~15–40 ms, app actions instant. Governed by the
internet line, identical across tiers.

**Voice commands — queue under burst.** The GPU processes commands in sequence; a burst queues
(nothing dropped). Latency of the last command in the queue:

| | Current 6 GB laptop | Starter | Mid | High |
|---|---|---|---|---|
| Single command (normal) | ~1–1.8 s | ~1 s | ~0.8 s | ~0.8 s |
| **Realistic worst case** (~30 at once) | ~27 s | ~6–12 s | ~3–6 s | ~2–4 s |
| **Absolute worst case** (all 200 in 1 s) | ~3 min | ~45–90 s | ~20–40 s | ~10–20 s |

Calls and app actions are a separate path — unaffected by voice-command burst.

---

## 4. Hosting cost estimates (Iran)

Monthly estimates, budget-friendly → best (confirm in rial — exchange-rate sensitive):

| Item | Spec | ~Monthly (USD-eq) |
|---|---|---|
| Object storage (saved clips) | S3-compatible, ~tens of GB | **~$5–15** |
| Cloud server (call/media traffic only) | 8 vCPU, 16–32 GB, static IP, symmetric domestic bw | **~$30–90** |
| Colocation (our hardware in a data center) | 1U–2U + power + symmetric domestic bw + static IP | **~$50–150** |

Domestic GPU-as-a-service is scarce, so GPU compute is the one-time hardware cost in §2 rather
than a monthly line.

**Provider reference:**

| Provider | Offers |
|---|---|
| Asiatech / Pars Pack / IranServer | Colocation + business bandwidth |
| ArvanCloud | Cloud servers, CDN, DDoS protection, object storage |
| Liara | Managed app / database hosting |
