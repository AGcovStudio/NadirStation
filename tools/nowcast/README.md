# GK-2A cloud-top nowcast (AI 外推) — training

The application runs the model offline (src/nowcast.cpp reads resources/models/nowcast_v1.txt).
These scripts train it from the station's own GK-2A receptions and are the reference the C++ code
mirrors step for step (the C++ result was checked against them: identical RMSE on the test case).

- nowcast.py   motion (half-resolution block matching), semi-Lagrangian advection, the 8 features
- train.py     trains the 8-24-24-1 MLP on consecutive 10-minute triples, writes nowcast_v1.txt
- evalnc.py    persistence / advection / advection+trend / model, RMSE and cold-cloud CSI
- export.py    writes the model in the text format src/nowcast.cpp reads

Inputs are FY2_DATA/GK-2A/<yyyyMMdd_HHmmss>/GK2A_IR_0p01K.png converted to <name>.npy (uint16).
Needs numpy, scipy, scikit-learn, Pillow. Retrain when more consecutive observations exist: list the
triples (t-10, t, t+10 minutes) in train.py.

Result of v1 (2026-10-19): held-out triple 2026-10-07 05:20/05:30 -> 05:40, model trained without it:
RMSE persistence 5.05 K, advection 3.73 K, model 3.40 K; CSI(<240 K) 0.74 / 0.81 / 0.83.
