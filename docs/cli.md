# `fabobs` and the stdio protocol

## Commands

| Command | Purpose |
|---|---|
| `fabobs version` | version, observation schema, journal format version |
| `fabobs policy` | the effective deterministic policy, its digest, and the well-known aspect table |
| `fabobs ingest` | ingest one observation (`--observation`) or a line delimited batch (`--observations`) |
| `fabobs snapshot` | print the current snapshot as JSON |
| `fabobs query` | filter the current snapshot; `--text` for the canonical summary |
| `fabobs hierarchy` | the observed containment tree, with unattached entities named |
| `fabobs diff` | diff two retained snapshots, or `--lookback N` |
| `fabobs history` | list retained snapshots |
| `fabobs explain` | why an aspect has the truth state it has |
| `fabobs sources` | source lineage, declared authority, freshness |
| `fabobs verify` | journal recovery report; exits non-zero when the journal is damaged |
| `fabobs serve --stdio` | the newline delimited JSON protocol |

## Common options

| Option | Effect |
|---|---|
| `--journal PATH` | journal file; enables persistence and recovery |
| `--fabric NAME\|fab:HEX` | fabric identity; a name is derived as `fabric/<name>` |
| `--pretty` | pretty print JSON |
| `--no-claims` | omit per-claim evidence |
| `--out PATH` | write output to a file |
| `--evaluation-time NANOS` | pin the instant freshness is evaluated at |
| `--clock system\|manual[:NANOS]` | clock source. `manual` makes a replay bit-for-bit reproducible across processes |
| `--text` | canonical text output where the command supports it |

Because receive times are part of the evidence, two runs only agree on snapshot
identity if they agree on when they received. `--clock manual:<nanos>` is what
makes cross-process reproducibility possible, and it is how
`process_transport.two_processes_agree_on_snapshot_identity` is proved.

## Query options

`--kind KIND`, `--domain DOMAIN` (repeatable), `--truth STATE` (repeatable),
`--aspect ASPECT`, `--subtree ENTITY`, `--limit N`, `--max-depth N`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | usage error, unknown command, missing option |
| 2 | operational failure: refused input, damaged journal, unavailable history |

## Integer encoding

64-bit integers are carried as **decimal strings** in JSON, because JSON numbers
are IEEE-754 doubles and a nanosecond timestamp does not round-trip through one.

Values are carried as tagged envelopes so the canonical value model survives the
round trip losslessly:

    {"k":"null"}
    {"k":"bool","v":true}
    {"k":"int","v":"-5"}
    {"k":"uint","v":"18446744073709551615"}
    {"k":"real","v":1.5}
    {"k":"text","v":"up"}
    {"k":"blob","v":"deadbeef"}
    {"k":"list","v":[{"k":"int","v":"1"}]}
    {"k":"map","v":[{"k":"a","v":{"k":"bool","v":true}}]}

## The stdio protocol

One complete JSON request per line, one complete JSON response per line, no
framing beyond the newline. The server reads a bounded amount per line: a client
that never sends a newline is refused with `TooLarge` rather than buffered.

Requests: `ping`, `ingest`, `ingest_batch`, `snapshot`, `refresh`, `query`, `hierarchy`, `diff`, `history`, `explain`, `sources`, `stats`, `recovery`, `summary`, `policy`, `shutdown`.

    {"op":"ingest","observation":{ ... }}
    {"op":"refresh","at":"1000000000000"}
    {"op":"query","filter":{"kind":"device","truths":["known"]}}
    {"op":"diff","lookback":"1"}
    {"op":"explain","subject":"device/dev:...","aspect":"link.state"}

Responses:

    {"ok":true,"op":"ingest","result":{ ... }}
    {"ok":false,"op":"ingest","code":"FencedSequence","message":"..."}

A fenced or rejected observation is a normal, reported outcome rather than a
protocol failure: the response is still an ok response carrying the disposition,
which is what makes disagreement visible to the caller.

## Example session

    fabobs ingest --observations observations.ndjson --journal fab.journal --fabric lab
    fabobs snapshot --journal fab.journal --fabric lab --evaluation-time 1000000000000
    fabobs query --journal fab.journal --fabric lab --truth conflicting --text
    fabobs explain --journal fab.journal --fabric lab         --subject device/dev:... --aspect link.state --text
    fabobs verify --journal fab.journal --fabric lab

Each command is its own process, and therefore its own restart: the evidence is
recovered from the journal and is never presented as fresh. That is the
conservative restart guarantee, observable from the command line.
