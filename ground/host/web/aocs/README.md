# Slot da interface do AOCS

A página da equipe do AOCS entra nesta pasta e aparece na aba **AOCS** da
estação de solo, dentro de um `iframe` servido pelo mesmo servidor
(`ground/host/gs_web.py`). O contrato:

- **Arquivos:** `index.html` e o que ele carregar, nesta pasta. Servidos em
  `/aocs/`. Tipos aceitos: `.html`, `.css`, `.js`, `.json`, `.png`, `.svg`,
  `.woff2`, `.txt`. Nada de CDN: no local da competição não há internet.
- **Estado do satélite:** `GET /api/state` (JSON) ou `EventSource('/events')`
  (o mesmo JSON, empurrado; a partir da segunda mensagem o campo `log` traz só
  as entradas novas). A atitude vem no HK: `hk.roll_cdeg`, `hk.pitch_cdeg`,
  `hk.yaw_cdeg`, em centésimos de grau, `null` sem dado. O modo do OBC em
  `mode`; as execuções de cada comando de modo em `runs`.
- **Comandos:** `POST /api/cmd` com `{"cmd": "mode aocs", "armed": true}`. O
  servidor confere tudo de novo (`check_command`): `mode aocs` exige
  `"armed": true`.
- **Identidade visual:** pode carregar `/css/gama.css` para usar as mesmas cores
  e a fonte (variáveis `--brg`, `--forest`, `--yellow`, `--terra`, `--panel`,
  `--fg`, …; tema em `document.documentElement.dataset.theme`).

Até o código chegar, a aba mostra a atitude do HK e o passo a passo do
`ADVANCED_AOCS`.
