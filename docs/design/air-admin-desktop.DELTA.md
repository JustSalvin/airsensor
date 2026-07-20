# "Air Admin Desktop.dc.html" — delta rispetto a `air-admin.dc.html`

Il mockup desktop del pannello admin (progetto Claude Design
`e7167602-ca9e-459a-bb16-5a86cfe13c75`, file "Air Admin Desktop.dc.html")
è identico al mobile in ogni sezione, componente, label e valore CSS.
Le UNICHE differenze (verificate sul file remoto, che non è duplicato qui):

1. Contenitore: `max-width:920px; padding:28px 24px 40px` (mobile: 430px,
   `20px 18px 40px`).
2. Dopo l'header, il corpo è avvolto in una griglia a due colonne:
   `display:grid; grid-template-columns:1fr 1fr; gap:14px; align-items:start`.
   - Colonna sinistra (`display:flex;flex-direction:column;gap:14px`):
     sezioni **CO₂ calibration**, **Environment compensation**, **VOC tuning**.
   - Colonna destra (stesso flex): **NOx tuning**, **Maintenance**,
     **General**, **Device**.
3. Il link header "← Dashboard" punta alla dashboard desktop (nella nostra
   implementazione è sempre `/`).
4. Confirm dialog e toast restano fuori dalla griglia, invariati.
5. Header identico, incluso il sottotitolo "Living room · SEN66" →
   NOTA IMPLEMENTAZIONE (vale anche per il mobile): il sottotitolo diventa
   solo "SEN66"; la sezione **General** (Room name) NON va implementata.

Implementazione: stessa pagina `/admin`, media query `min-width:700px` circa
che attiva la griglia a due colonne con questa distribuzione delle sezioni.
