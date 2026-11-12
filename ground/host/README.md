# Ground station no PC

O que roda no computador da ground station, em Python:

- `gs_serial.py`: a ponte com a ESP32 da ground (ADR-0018). Reseta a placa, dá a ela o relógio do PC (a ESP32 não tem), manda os comandos e grava as linhas JSON. Para os roteiros da bancada, a ESP32 fica igual à `gs_cli`. Uso em `ground/esp32/README.md`; testado contra uma ESP32 falsa num pseudo-terminal (`test_gs_serial.py`, CTest `gs_serial`).
- `gs_web.py` (e `gs_web_ui.html`): a interface da ground no navegador. Segue o satélite ao vivo (modo do OBC, tensão, corrente, temperaturas, enlace, linha do tempo de eventos) e manda os comandos pelos botões. Veja "A interface no navegador" abaixo.
- `tracks_to_ndjson.py`: do log da ground ao formato do estimador (abaixo).
- `ground-aeronaves/`: o projeto ADS-B da ground, trazido da branch `ADSB-ground` (onde ficava em `ground- adsb/`): recepção de NDJSON, estimador por aeroportos/pistas, memória temporal, painel web, simulador e testes.
- `banco-aeronaves/`: importador do cadastro OurAirports. O banco de rotas foi removido.

## Executar

Requer Python 3.10 ou mais recente; o núcleo usa a biblioteca padrão.

```bash
cd ground/host/ground-aeronaves
python3 -m venv .venv
source .venv/bin/activate
python -m unittest discover -s tests -v
```

Os mesmos testes rodam no `make test` (CTest `ground_aeronaves`).

Para simular ou usar o cadastro geográfico, coloque uma cópia do `referencias.db` local em `ground/host/banco-aeronaves/`. Alternativamente, copie os CSVs e o manifesto do cadastro local para `banco-aeronaves/fontes/` e execute `python3 banco-aeronaves/importar_bases.py` a partir desta pasta. O importador verifica os hashes e recusa sobrescrever tabelas existentes. O banco fica ignorado pelo Git.

Com o banco instalado, execute em dois terminais dentro de `ground-aeronaves/`:

```bash
python simular_trajetorias.py
```

```bash
python -m ground.visualize --serve
```

O simulador e o painel usam por padrão `~/adsb/eventos.ndjson`. Para processar um arquivo até o fim: `python -m ground --input /caminho/eventos.ndjson`.

## A interface no navegador

`gs_web.py` lê as mesmas linhas JSON da ground e serve uma página com o estado do satélite. Só biblioteca padrão; escuta em `127.0.0.1:8080`.

Pela raiz do repositório, `make gs-web` escolhe a fonte por variável: `ESP32_PORT=/dev/ttyUSB0` (padrão), `UDP=1` ou `REPLAY=<log.jsonl>` (com `SPEED=10`). `POWER`, `WEB_HOST` e `WEB_PORT` ajustam a potência da ground, o endereço e a porta. Equivale a:

```bash
python3 gs_web.py --serial /dev/ttyUSB0 --power 2        # a ESP32 da ground
python3 gs_web.py --udp                                   # gs_cli no rádio UDP (build/ primeiro)
python3 gs_web.py --replay ../../docs/vv/bancada/AAAA-MM-DD/x-ground.jsonl --speed 10   # sem hardware
```

Abra `http://127.0.0.1:8080`. A página mostra o modo atual no diagrama da Figura 44 (clicar num modo manda o `SET_MODE`), a telemetria com "sem dado" no lugar dos valores sentinela, o enlace (RSSI, SNR, ACKs, latência) e a linha do tempo. Português, inglês e espanhol; o botão "projetar" aumenta as letras.

- **Só os comandos de operação passam:** `ping`, `hk`, `stat`, `roster`, `time`, `stream`, `stop`, `rate` e `mode`. `shutdown`, `deafen` e o resto do console são recusados no servidor.
- **`mode basic_inter` e `mode aocs` pedem para armar** (o burn-wire e o empréstimo do I²C ao AOCS). A chave de armar na página desliga sozinha em 15 s e depois de cada disparo, e o servidor confere de novo.
- Com `--replay` a página avisa que é demonstração e que os comandos não chegam a lugar nenhum: serve para ensaiar a apresentação.
- `--host 0.0.0.0` abre a página para a rede; não há senha, então só numa rede da equipe.

Os testes (`make test T=gs_web`) cobrem o estado, a lista de comandos e o servidor contra uma ground falsa.

## Do downlink ao estimador

`tracks_to_ndjson.py` converte o log da ground station (um JSON por evento, como o `tools/gs_cli` escreve e como a ESP32 manda pela USB, pelo `gs_serial.py`) no NDJSON que o estimador lê: um objeto por registro de aeronave recebido, com o horário refeito no relógio desta máquina — chegada menos tempo no ar menos idade, ADR-0007 — e só os campos que o registro marca como válidos. O teste dele passa cada linha pelo `parser.py` do estimador.

```bash
gs_cli --radio udp ... | python3 tracks_to_ndjson.py | (cd ground-aeronaves && python -m ground --input -)
python3 gs_serial.py --port /dev/ttyUSB0 --auto-time on | python3 tracks_to_ndjson.py | (cd ground-aeronaves && python -m ground --input -)
```

A cadeia inteira sem hardware — captura ou simulação, `adsbd`, `ttcd`, rádio UDP, ground e estimador — é o `make adsb-chain INPUT=...`, descrito em `flight/adsbd/README.md`.

## Avaliação e dados locais

```bash
python validation/gerar_casos_avaliacao.py
python validation/rodar_teste_completo.py
python validation/diagnosticar_estimador.py
```

Os comandos acima exigem o cadastro geográfico. Os casos sintéticos e as respostas esperadas são gerados pelo primeiro comando. `validation/evaluate.py` é uma avaliação histórica que exige o pacote local `validation/baseline-2026-09-24/`; ele não é distribuído aqui. O teste E480F9 é ignorado se o banco ou sua captura local não estiverem presentes.

Não são versionados bancos, capturas NDJSON, dados de referência, estados gerados, pacotes históricos ou ambiente virtual. Para reproduzir exatamente uma avaliação anterior, use os mesmos dados locais. A documentação histórica pode mencionar caminhos do computador original e etapas antigas; este arquivo descreve a organização para GitHub.

As estimativas são heurísticas experimentais. TENTATIVE é uma hipótese provisória; ESTIMATED não confirma o evento nem representa uma probabilidade calibrada.

## Estimador com pares e conclusão combinada

A atualização está em `ground-aeronaves/`; `iniciar_painel.sh` inicia o painel.
O banco público de referências foi incluído. O banco de pares (~710 MB) é gerado
localmente, não versionado. Na raiz do repositório:

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r ground/host/ground-aeronaves/requirements.txt -r ground/host/pares/requirements-regional.txt
python3 ground/host/pares/gerar_pares.py --source ground/host/banco-aeronaves/referencias.db --output ground/host/pares/pares_aeroportos.db
python3 ground/host/pares/prioridade_regional.py
python3 ground/host/pares/auditar_cidades.py
./ground/host/iniciar_painel.sh --input /caminho/para/eventos.ndjson
```

O gerador recusa sobrescrever um banco existente. A prioridade regional é
necessária antes da análise; a auditoria documenta a cobertura sem alterar a
seleção. Veja os detalhes e limites nos READMEs dessas pastas. A execução requer
NumPy; Shapely é usado somente na geração regional. Os pesos da conclusão são
heurísticos, não probabilidades calibradas. Não há alteração do protocolo LoRa.
