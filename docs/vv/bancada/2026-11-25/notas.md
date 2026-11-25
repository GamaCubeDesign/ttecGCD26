# 25/11 — o enlace pelo SSH, o plano B do LoRa

O LoRa do satélite falhou e pode não voltar: o rádio A não responde
(`radio_open_failed: unexpected RegVersion`), e o `ttcd` no LoRa reinicia sem
parar. O plano B é o enlace pelo SSH (`./gama service link ssh`,
`./gama ground --ssh`). Hoje ele foi validado de ponta a ponta, primeiro no PC e
depois na Pi.

**Montagem:** só a placa do OBC (`obc-v2`) com o INA219, sem SDR, sem
bateria e sem a ESP32 no PC. **Rede:** o hotspot do celular (PC 172.20.10.8,
Pi 172.20.10.7). O hotspot tinha internet, então os relógios do PC e da Pi
estavam no NTP, e foi possível medir a hora do satélite contra uma referência.

## O caminho

```
ttcd ─ rádio UDP ─ 127.0.0.1:47001↔47002 da Pi ─ gs_cli (o console da ground) ─ ssh ─ gs_web.py no PC
```

O `ttcd` e o console da ground são os mesmos do LoRa. Também são iguais os
frames, o CRC, os ACKs, as retransmissões e as janelas half-duplex. O rádio
UDP (`flight/radio/radio_udp.c`) segura cada frame pelo tempo no ar e fica
surdo enquanto transmite. O ssh só leva as linhas JSON e os comandos em texto,
como o cabo USB faz com a ESP32.

Diferenças em relação ao LoRa:

- não há perda, ruído nem distância;
- o RSSI e o SNR são fixos (−50 dBm, 10 dB);
- a potência não tem efeito.

**Nada do que passa pelo SSH é evidência de RF.**

## Ensaio no PC, antes da Pi

Os programas de verdade rodaram no PC:

- a página (`gs_web.py --ssh`);
- o `ttcd` com os argumentos exatos do drop-in do `gama-link`, sobre o
  `flight/conf/obc-v2/ttcd.conf`;
- o `obcd`, o `adsbd` e o `sbs_replay`, com 23 aeronaves
  (`docs/vv/trafego-real/sintetico-rotas`).

No lugar do ssh, um substituto executou o comando remoto que a página monta.

| Teste | Resultado |
|---|---|
| `ttcd` sem ground escutando, 37 s | vivo, 4 frames enviados, sem erro (`sendto` num socket não conectado ignora o ICMP) |
| contato, `SET_TIME`, `ping 5`, `hk`, `time` | todos na 1ª tentativa; ping 268–269 ms, `SET_TIME` 310 ms |
| `mode mission_adsb` com `MISSION_S` de 120 s | tracks com a categoria, aeronaves na página, a missão termina sozinha, roster no fim |
| ssh derrubado no meio da missão | `SET_TIME` aceito de novo 7,9 s depois; a missão segue |
| `gs_cli` órfão segurando a porta 47002 | o `pkill` da reconexão o tira |
| `ttcd` reiniciado com a página aberta | `obcd` e `adsbd` reconectam, HK de volta em ~10 s |

## Na Pi

### A troca LoRa → SSH pelo serviço

- **Antes:** a Pi tinha acabado de ligar, no LoRa. Em ~1 min de boot, o `ttcd`
  somava 24 reinícios (`RegVersion`), e o `adsbd` não achava o socket do `ttcd`
  (`ipc_connect_failed`).
- **A regra do sudoers existe:** `sudo -n /usr/local/sbin/gama-link status`
  responde sem senha.
- **❌ A primeira troca falhou.** O `sudo -n gama-link ssh`, o mesmo comando do
  botão da página, gravou o drop-in, mas o systemd recusou o `restart`: `Job
  for ttcd.service failed because start of the service was attempted too
  often`. Saiu `RESULT ssh failed`, e o `ttcd` ficou em `failed`
  (`Result=start-limit-hit`), sem LoRa e sem SSH.
  - **A causa:** o `ttcd` no LoRa reinicia a cada `RestartSec` (2 s), perto do
    limite de partidas do systemd (5 em 10 s). O `restart` foi uma partida a
    mais, e numa unit em `failed` o `Restart=always` não tenta de novo.
  - **Por que não foi visto antes:** o `test_gama_link` usa um `systemctl` de
    mentira, que não tem esse limite.
- A segunda tentativa, ~1 min depois, deu `RESULT ssh active`: `ttcd` com
  `"radio":"udp"` em 127.0.0.1:47001, `obcd` e `adsbd` conectados.
- **Correção:** o `gama-link` chama `systemctl reset-failed ttcd` antes do
  `restart`, o que zera o contador. O `test_gama_link` agora simula o limite e
  falha com o script antigo, com a mesma saída da Pi (`RESULT ssh failed`).
- O `./gama service link ssh` sem terminal recusa antes de tentar (`o sudo
  precisa de um terminal`), embora o sudo da Pi não peça senha. Num terminal,
  ele roda.

### Operação pela página (`./gama ground --ssh --no-build`)

Sessão em [`ssh/ground.jsonl`](ssh/ground.jsonl): três inícios (o primeiro,
depois da queda e depois do reboot), 32 ACKs.

- **Contato:** `SET_TIME` na 1ª tentativa, 309 ms.
- **`ping 5`:** 4 na 1ª tentativa (268–269 ms), 1 com 2 tentativas (971 ms).
- **`hk`:** `REQ_HK` na 1ª tentativa, 269 ms.
- **`mode mission_adsb`:**
  - `SET_TIME` e `SET_MODE` na 1ª tentativa (310 e 289 ms);
  - `MISSION_ADSB` até o `ADSB_TIMEOUT`, aos 600 000 ms;
  - depois `MISSION_DOWNLINK`, e de volta a `PRE_TEST` 716 ms depois
    (`TASK_DONE` do `ttcd`);
  - stat e roster finais chegaram à página;
  - 29 ACKs OK, nenhum ruim, nenhum TC perdido.
- **Sem SDR:** o `dump1090-fa` sai com `rtlsdr: no supported devices found`, e
  o `adsbd` o reinicia a cada 1 s. Zero aeronaves, como esperado sem o SDR.
- **`mode task_done` durante a `MISSION_ADSB`:** o ACK veio OK (o `ttcd`
  recebeu e repassou), mas o `obcd` recusou (`event_refused`, `TASK_DONE` em
  `MISSION_ADSB`), como manda a Figura 44: a missão termina pelo tempo. A
  página mostra "SET_MODE: OK" mesmo assim.
- **Keepalive durante o STREAM:** os PINGs da ground levaram até 7,2 s. É o
  half-duplex: a ground fala enquanto o satélite está no ar com tracks.

### Queda do ssh e reboot

- **ssh morto no meio da missão (`kill -9` no PC):**

  | Evento | Tempo |
  |---|---|
  | aviso na página | +0 s |
  | ssh de novo | +9,4 s |
  | contato | +12,9 s |
  | `SET_TIME` aceito, na 1ª tentativa | +13,2 s |

  A missão seguiu.
- **Reboot da Pi com a página aberta:**

  | Evento | Tempo |
  |---|---|
  | ssh de novo | +39 s |
  | contato | +47,8 s |
  | `SET_TIME` aceito, na 1ª tentativa | +48,3 s |

  O `ttcd` subiu direto no UDP pelo drop-in (`NRestarts=0`). O drop-in
  sobrevive ao boot.

### ❌ A hora do satélite atrasava de 0,5 a 4,4 s

A página lia a hora do PC (`--clock`) quando o ssh começava. O `gs_cli` a
recebia depois do tempo de conexão do ssh pelo Wi-Fi e, numa reconexão, depois
também do `pkill` e do `sleep 0.5`. Com esse atraso, ele mandava ao satélite
um `SET_TIME` atrasado. O registro de bordo fica com esse atraso, e as
trajetórias da ground também, porque elas usam o `wall_ms` do `gs_cli`
(`tracks_to_ndjson.py`).

Âncoras do `obcd` e do `adsbd` contra o NTP da Pi
([`analise/ancora_ntp.py`](analise/ancora_ntp.py)):

| Momento | Satélite atrás da hora real |
|---|---|
| 1º contato | 0,54 s |
| reconexão no meio da missão | **4,40 s** |
| depois do reboot | 3,06 s |

Com a página fechada, [`analise/relogio_ssh.py`](analise/relogio_ssh.py), com
a versão antiga da página, deu:

- 5 conexões: 561, 677, 719, 960 e 1867 ms;
- 3 conexões com um `gs_cli` órfão na Pi: 1065, 1125 e 1242 ms.

O HLR-ADS-07 conta 100 ms no enlace, ≈ 23 m a 450 kt. Os 4,4 s dariam ~1 km.
Pelo LoRa isso não acontece.

**Correção:** a hora passa a ser lida quando a Pi a pede. O comando remoto faz
o `pkill`, imprime `@clock?` e lê uma linha. A página (`SshSource`) responde
com a hora do PC naquele instante, e o `gs_cli` parte com ela. Sobra a latência
de ida pelo Wi-Fi. Até a hora ser entregue, a página recusa comandos ("the
ground is not running"); senão, o shell da Pi tomaria o comando pela hora. No
PC, com 2 s de conexão simulados:

| Versão | Sem órfão | Com `gs_cli` órfão |
|---|---|---|
| antiga | 2035–2039 ms | 2535–2542 ms |
| nova | 1 ms | 1–2 ms |

### Fora do SSH

- **INA219:** neste boot, `Input/output error` em todo ciclo (`power_failures`
  sobe 1 por segundo; o `obcd` tenta de novo a cada ciclo). Num boot anterior
  de hoje ele leu ~7,0 V e 161–249 mA. Ver o SCL em 0 V de 22/11
  (`docs/relatorios/2026-11-22-i2c-scl-em-curto.html`).
- **DS18B20:** `NOT_FOUND`.

## Pendências

- [ ] Na Pi: instalar o `gama-link` corrigido (`make install` ou
      `sudo install -m 0755 flight/ttcd/gama-link /usr/local/sbin/gama-link`)
      e refazer a troca a partir do LoRa em loop (`gama-link lora`, esperar
      ~15 s, `gama-link ssh`): ela tem de passar na 1ª vez.
- [ ] Na Pi: `analise/relogio_ssh.py` e `analise/ancora_ntp.py` com a página
      nova, sem órfão e com `--stray`.
- [ ] `./gama service link` sem terminal: usar `sudo -n`, que a regra do
      sudoers permite, em vez de recusar.
- [ ] A página dizer quando o OBC recusa um `SET_MODE` (o `refused` do `obcd`
      não chega à ground).
- [ ] Uma página por vez contra a mesma Pi: duas derrubariam o `gs_cli` uma da
      outra a cada reconexão (deduzido do código, não testado).
