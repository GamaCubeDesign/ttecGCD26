# 24/11 — teste de mecanismos (REG-07)

Satélite montado (`obc-v2`), ground na ESP32 pelo PC, rede pelo hotspot.

## Burn-wire

- 5 s, 10 s e 2 s com o R2 de 4,7 kΩ da placa do OBC: o fio esquentava e
  não rompia o nylon (base do TIP120 em ~0,25 mA, ≤ 0,25 A no fio; achado A5).
  Relatório: `docs/relatorios/2026-11-24-resistor-burn-wire.html`.
- Depois de um pulso de 2 s, o coletor do TIP120 ficou vermelho com o GPIO22
  já desligado (o log do `obcd` confirma o fim do pulso): curto fora do
  software. Hipóteses: TIP120 em curto C–E, o multímetro na entrada A/mA ou
  uma ponta curto-circuitando o NiCr, um curto na placa ou no cabo.
- R2 retirado e substituído por uma ponte de estanho (base de volta aos
  ~8,5 mA do ensaio do CDR); `burnwire_ms = 750` em
  `flight/conf/obc-v2/obcd.conf`. **Com isso o mecanismo funcionou.**

## Enlace

- Depois de reconectar a ground, os PINGs falharam por ~2,5 min: o satélite
  tinha caído para SAFE (120 s sem contato) e a ESP32 voltou em NOMINAL. A
  ground cai para SAFE depois de 150 s e o enlace se refaz sozinho. Depois de
  reconectar a ground, esperar o "contact" na página.

## Pendências para depois da prova

- [x] **Página da ground, aba Deployment: os passos 3 a 5 não avançam.** A
      página só sabe o modo pelo HK (a cada 10 s em NOMINAL), e
      `BASIC_INTERMEDIATE` dura `burnwire_ms` (750 ms): nenhum HK pega o modo,
      e a página fica em "OBC no modo BASIC_INTERMEDIATE" embora o pulso tenha
      acontecido (o ACK do `SET_MODE` chegou; o `obcd.jsonl` registra o pulso).
      Caminho proposto: o `ttcd` manda um HK assim que recebe `obc_mode` do OBC
      (`flight/ttcd/tm_sched.c`), para a ground ver toda transição; refazer
      `test_link_sim` e a tabela do ADR-0007, que isso muda o tráfego.
      Alternativa só na página: considerar o deployment feito quando o modo
      volta a PRE_TEST depois de um `SET_MODE basic_inter` aceito.
      Feito em 25/11 pela alternativa só na página (`gs_web.py`,
      `SHORT_MODES`): o passo aparece como "visto pelo ACK", sem duração.
- [x] O texto da aba ainda diz "por 5 s" (`ground/host/web/js/i18n.js`,
      `s_burn` e `deploy_lead`): trocar pelo valor da Pi ou por "um pulso de
      `burnwire_ms`".
- [x] Na Pi, `./gama board install`: `/etc/gama/obcd.conf` só com
      `burnwire_ms = 750`, e o boot do `obcd` registra 750.
