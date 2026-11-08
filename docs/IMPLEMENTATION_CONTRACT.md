# Implementation contract

Qt 6.5+ / C++17. This project contains the Widgets client; graphical/headless simulation belongs to the independent sibling StationSimulator project. The applications communicate through real TCP with a versioned CRC-protected JSON frame. Default builds and tests do not require the other project. No existing user data or credentials are copied. Business package responsibilities and build dependencies are documented in [PROJECT_STRUCTURE.md](PROJECT_STRUCTURE.md).

## Shared JSON contracts

Recipe: `{version,name,steps:[{id,label,unit,min,max,nominal}]}`. Published versions are immutable, ordered steps unique, finite numeric min <= nominal <= max; 1..16 steps. Default recipe version `demo-5v-v1` contains voltage (4.75..5.25 V, nominal5), standby current (10..80 mA, nominal35), IO (1..1, nominal1). Limits are demonstration data.

Run request: `{runId,stationId,sn,recipeVersion,recipe}`; runId is UUID, stationId1..4, SN trimmed1..64 printable characters. Repository reserves run BEFORE sending a start command. One unfinished run per station and per SN.

Terminal result: `{runId,stationId,sn,recipeVersion,outcome,steps:[{stepId,value}]}`; outcome `completed` or `cancelled`. On completed, exact ordered recipe steps are required. Cancelled may contain a valid prefix. Reject nonfinite/out-of-order/unknown steps and identity mismatch. Derive PASS/FAIL/CANCELLED from recipe limits, never trust remote verdict. One terminal per runId; canonical-equivalent duplicates succeed without extra rows; conflicting terminal rejected. ResultAck is sent only after successful transaction. Failed saves retain pending result and retry, block station reuse. Simulator persists pending terminal across restart and sends until resultAck. Store result+steps+verdict atomically.

Wire object: `kind` (`hello`, `helloAck`, `ping`, `pong`, `start`, `accepted`, `progress`, `terminal`, `resultAck`, `cancel`, `query`, `runState`, `error`), `session` (connection UUID), `sequence` (positive integer). Replies echo relevant sequence/session. Start carries `run` object; query/cancel/resultAck carry `runId`; terminal carries `result`; progress carries `runId,stepIndex,stepId,value`; runState carries `runId,state` with executing/terminal/unknown and may include `result`. Only same-session messages accepted. Simulator supplies stationId on helloAck; client verifies endpoint identity. A run persists across TCP reconnect; unknown run after reconnect becomes INTERRUPTED, not auto-restarted.

## Protocol module

`src/shared/protocol/framecodec.h/.cpp`: namespace Wire, `QByteArray encode(const QJsonObject&)`; `Parser::append(QByteArray)->QList<QJsonObject>`, `takeError()->QString`, `reset()`, `bufferedBytes() const`; bounded parser, reject unsupported version, lengths, CRC, malformed JSON, recover after garbage. `tests/shared/test_protocol.cpp` uses QtTest.

## Storage module

`src/traceability/repository.h/.cpp`: QObject `Repository`, methods/slots `initialize(QString databasePath)`, `reserveRun(QJsonObject request)`, `saveResult(QJsonObject result)`, `queryHistory(QString snFilter,int beforeId=0)`, `listRecipes()`, `publishRecipe(QJsonObject recipe)`, `markInterrupted(QString runId,QString reason)`, `shutdown()`.

Signals: `initialized(bool ok,QString error)`, `runReserved(QJsonObject request,bool ok,QString error)`, `resultSaved(QJsonObject result,bool ok,bool duplicate,QString error)`, `historyReady(QJsonArray rows,bool more)`, `recipesReady(QJsonArray recipes)`, `operationError(QString message)`.

History rows keys `id,runId,stationId,sn,recipeVersion,execution,verdict,startedAt,finishedAt,reason,steps` (ordered step rows with stepId,label,value,unit,min,max,passed). Page 100+1, beforeId cursor. Single DB connection in owning thread with WAL, foreign keys, busy_timeout250ms. After process restart mark unfinished runs interrupted, preserve terminal data. Tests in `tests/traceability/` use temporary DB, verify actual rows/triggers/locks/restart/duplicates/recipe immutability. Shared domain validation/hash belongs to the testing business package in `src/testing/domain.h/.cpp`.

## Testing and workbench application modules

`src/testing/testengine.h/.cpp` owns the network execution state machine and emits persistence requests. Its connection value type is declared in `src/testing/stationendpoint.h`; the engine does not include a workbench controller, repository or widget. `src/workbench/application/workbenchcontroller.h/.cpp` owns application coordination, creates the network/database/log workers and connects their requests and results. Shared asynchronous logging is in `src/shared/logging/`.

## GUI module

`src/workbench/ui/mainwindow.h/.cpp` constructor `MainWindow(const QString &dataDirectory, const QList<StationEndpoint> &endpoints, QWidget *parent=nullptr)`; owns WorkbenchController and shuts down before destruction. `src/workbench/ui/models.h/.cpp` typed Qt models use QAbstractTableModel/QSortFilterProxyModel/delegates, never SQL in GUI. WorkbenchController public interface is declared in `src/workbench/application/workbenchcontroller.h`; the operator entry point is `src/apps/workbench/main.cpp`. Client style belongs to `src/shared/widgets/`; the independent simulator maintains its own frontend.

UI: station execution board, product SN and recipe selection, start/cancel, step/result matrix, trace history by SN plus next cursor, per-run historical details, publish new recipe version by JSON file with validation, log. Clearly show execution/verdict/stored separately; simulation indication, no optimistic completed on click. Display one station selection shared by command and details. No GUI-side blocking files for log/history; file recipe input small bounded configs only. Signals carry QJson values.

## Simulator

The sibling `D:\demoqt6\StationSimulator` owns `src/main.cpp`, `src/simulation/`, `src/contracts/`, `src/transport/` and `src/presentation/`. Its code and independent tests do not reference this client's source tree or Qt Sql. Namespaces `SimulatorDomain`, `SimulatorWire` and `SimulatorStyle` isolate internal symbols while preserving the same online contracts. Headless executable `StationSimulator --headless` accepts `--port`, `--station-id`, `--data-dir`, `--step-ms`, `--fail-step` (id, optional), `--drop-first-result-ack` (ignore first resultAck to force duplicate). One client control session; persistent run and terminal outbox via QSaveFile atomic JSON (bounded small state, not unlimited journal). Reply to query and cancellation; retain terminal until resultAck. Duplicate same run start does NOT execute again; conflict rejected. Startup with previously executing run terminates as interrupted/unknown (explicit response), never fabricates PASS. Simulator only, no physical electrical testing claim.

Each business package defines its own CMake targets. Package interface changes must update callers, the matching tests and this contract. Client root CMake assembles only client packages and installs StationTestWorkbench. Seven default test targets are self-contained. WORKBENCH_ENABLE_SIMULATOR_TESTS explicitly imports STATION_SIMULATOR_SOURCE_DIR for three additional cross-project test targets only. The simulator has a separate root CMake, four standalone test targets and its own deployment package.

## Final implementation additions

Schema version is 2. `recovery_pending` is included in station/SN admission indexes. Startup emits `recoverableRuns(QJsonArray fullRequests)` before initialized; network initializes with these identities and queries without replaying actions. `markInterrupted` emits `runInterrupted(runId,ok,error)`; identity is not released until this succeeds.

The simulator acknowledges durable resultAck receipt with `resultConfirmed {runId}`. Client blocks the next run until this reply or an authoritative reconnect reconciliation. Lost ACK is retried. Recently committed identical terminals are re-acknowledged; conflicts do not overwrite the original.

Repository and controller additionally expose `exportReport(QString runId,QString path)` / `reportReady(QString path,bool ok,QString error)`. SQL and QSaveFile HTML generation run in database thread. Report includes bound recipe hash/limits, terminal values and explicit unexecuted steps. History rows include `recoveryPending`. UI uses QualityDelegate for result badges and `exportReportButton` for selected run reports.

Repository and controller expose `exportFailedSns(QString path)` / `failedSnsExported(QString path,bool ok,qint64 count,QString error)`. Export all committed `COMPLETED/FAIL` runs, deduplicate by exact SN and stream a single-column UTF-8 BOM CSV through QSaveFile. Ignore UI filtering and pagination; later PASS retests do not remove historical failures. Cancellation, unknown and uncommitted runs are not failure records. The workbench uses `exportFailedSnsButton`, prevents duplicate export admission and reports success/count only after the file commit.

Manual `HistoryBenchmark` creates a 100000-run /300000-step synthetic fixture and measures actual Repository history queries on its worker thread. It is not registered as a routine CTest and does not measure production writes, GUI rendering, cold cache or long-duration stability.


Recipe preview: `recipePreview` carries a validated `recipe` in the verified station session. It is non-executing, per-station, deduplicated, and retransmitted after reconnect. Server snapshots expose it as `recipePreview`, independently from the persisted run `recipe`. Busy stations may cache the next preview, but active recipe/steps/results and fault settings remain unchanged. Fault choices switch to the preview only after the current run is reconciled.
