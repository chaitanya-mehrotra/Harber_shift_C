# HarborShift C

HarborShift simulates port disruptions, shipment delays, manufacturing dependencies, and recovery options using synthetic data. This Windows edition serves the React dashboard with a C backend. The original Python delay model and its training code are included.

## Run on Windows

Open PowerShell in this folder and run:

```powershell
.\run.ps1 -Port 8180
```

Open [HarborShift on port 8180](http://127.0.0.1:8180/). The C server serves the bundled dashboard and API together. The project ZIP includes a Windows executable. For a Git checkout, install GCC for Windows so `run.ps1` can build the executable on first launch. Run `./build.ps1` to rebuild after editing the C code. For Vite development, run the C server on port 8000 and use the frontend's existing Vite commands.

### Optional Python delay model

Run these commands once from the project folder to enable ML estimates for restricted port calls:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements-model.txt
```

The C server starts the predictor on the first ML request. It uses the project's `.venv` when present and otherwise looks for `python` on PATH. Set `HARBORSHIFT_PYTHON` to select a different executable. The simulator uses C rules for closures and when ML is unavailable. To retrain the original model, run `.\.venv\Scripts\python.exe -m harborshift.ml.train`.

## Start a simulation

The welcome screen opens without a route or dashboard. Choose **Start simulation** to open Overview on the planned voyage with no disruption applied. Overview lists the manufacturing milestones and shows revised dates after applying an event. Use **Replay voyage** to replay the route animation. To try the Durban strike, choose **Simulate a disruption**, then **Apply event** with preset E001 selected.

## Watch Dijkstra search on the globe

In **Network**, click **Find route**, then **Watch the search on the globe**. Play the search or move through it one step at a time. Each step shows the port popped from the C min-heap and the routes that improved a tentative estimate. Try closing Durban (P05) and replaying the search to see how the explored ports change.

## Demonstrate the data structures

```powershell
.\build\harborshift.exe validate
.\build\harborshift.exe neighbors P03
.\build\harborshift.exe route P03 P11 --quantity 10 --units-per-container 10 --close-port P05
.\build\harborshift.exe simulate --manufacturing --as-of 2027-01-05 --event E001
```

The route command compares a **12-day** baseline with a **32-day** alternative when Durban is closed. The E001 manufacturing simulation moves customer delivery from **17 January to 19 January 2027**, with an estimated penalty of **₹5,000**. The route and simulation commands print JSON so you can inspect paths, dates, and cost calculations.

| C source | Data structures and algorithms |
|---|---|
| [backend/routing.c](backend/routing.c) | Directed graph in compressed adjacency lists and Dijkstra with a binary min-heap. Each heap-based search takes up to \(O((V + E)\log(E + 1))\) time after edges are filtered. |
| [backend/cascade.c](backend/cascade.c) | A task DAG, Kahn's topological sort with a FIFO queue, and BFS to trace the dependency paths that caused downstream delays. The adjacency matrix makes each traversal \(O(T^2)\) for \(T\) tasks. |
| [backend/impact.c](backend/impact.c) and [backend/scenario.c](backend/scenario.c) | Ordered itinerary replay, bit masks for active disruption tracking, and contract ranking by deadline slack and daily penalty. |
| [backend/recovery.c](backend/recovery.c) | Comparison of waiting, rerouting, and full-quantity sourcing. The small list of eligible recovery options uses insertion sort. |

The application routes are `GET /health`, `/ports`, `/routes`, `/shipments`, `/shipments/{id}/itinerary`, `/disruptions`, `/news` and `POST /route`, `/simulate`. The dashboard uses the same paths under `/api`.

## Data and project files

- `backend/` contains the C HTTP server, data validation, graph algorithms, simulation, recovery, and advisory RSS/Atom processing.
- `data/network.json` is the validated runtime snapshot. Run `./import-data.ps1` to rebuild it from the original ZIP, or `./import-data.ps1 -CsvDirectory "C:\path\to\csvs"` to use a CSV folder. Windows `tar.exe` extracts the bundled archive, and C validates and converts the CSVs. The manufacturing fixture is read by C when the server starts.
- `frontend/` contains the React dashboard, including its built assets and Dijkstra playback. The C server uses those assets directly.
- `harborshift/ml/` and `artifacts/` contain the original Python predictor, trainer, model and evaluation report, plus the small pipe adapter. The retained Python CSV modules are imported by the offline trainer.
- `backend/vendor/` contains cJSON 1.7.19 and Yxml with their licenses.

To run the original desktop and mobile dashboard tests against C, install the frontend test dependencies and run the alternate configuration from this folder. It serves the bundled frontend from the C server on port 5173. Close any existing process you started there before running this command.

```powershell
npm.cmd --prefix frontend ci
$env:HARBORSHIFT_BROWSER_CHANNEL = 'chrome'
.\frontend\node_modules\.bin\playwright.cmd test --config playwright.c.config.mjs
```
