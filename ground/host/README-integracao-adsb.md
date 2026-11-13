# Ground integrada ao ADS-B

Na raiz do repositório, com Python 3 e as dependências de
`ground/host/ground-aeronaves/requirements.txt` instaladas:

```sh
./ground/host/iniciar_ground.sh --serial /dev/ttyUSB0 \
  --pairs-db /caminho/pares_aeroportos.db
```

Abra `http://127.0.0.1:8080` e clique em **Iniciar missão ADS-B**.
O comando passa pela console compartilhada: primeiro SET_TIME, depois
SET_MODE somente após confirmação do horário. O processamento já está ativo
antes do envio. A interface mostra ACKs e falhas; enviar um comando não é
confirmação de que o OBC entrou no modo solicitado.

A ESP32 recebe LoRa e envia os eventos pela USB. Uma única conexão serial
alimenta a telemetria e o processamento ADS-B. A ground reconstrói o horário
dos registros, remove repetições, processa o método original e agenda o método
de pares nos processos existentes. O painel de aeronaves mostra os dois
rankings e a decisão combinada conforme a fase do voo e a evidência disponível.
Resultados insuficientes continuam pendentes, sem forçar uma escolha.

O banco de referências padrão é `ground/host/banco-aeronaves/referencias.db`.
Use `--reference-db` para substituí-lo. O banco de pares precisa ser fornecido
se não estiver em `ground/host/pares/pares_aeroportos.db`; o arquivo grande não
é distribuído pelo Git. A aplicação verifica as referências e os metadados de
conclusão dos pares antes de abrir a conexão. `--no-pairs` desativa explicitamente
a segunda estratégia, para diagnóstico.

Cada execução cria uma pasta exclusiva em `ground/host/sessions/` (ou
`--session-dir`), contendo `ground.jsonl`, `tracks.ndjson`, `telemetria.db` e
`estado.json`. Os bancos de referência permanecem somente leitura.
A recepção grava o log e uma thread independente consome esse registro;
a busca de pares roda em subprocessos. Registros rejeitados são contados e
armazenados no SQLite. Falhas fatais aparecem no painel e bloqueiam novo comando
de missão; o log recebido permanece disponível para investigação/reprocessamento.
A escrita em disco não usa fsync por mensagem: não há garantia contra perda de
energia abrupta. No encerramento normal, a aplicação termina a fonte, consome o
log restante e aguarda os pares dentro do prazo já configurado pelo estimador.

Para ensaiar sem hardware:

```sh
./ground/host/iniciar_ground.sh --replay /caminho/ground.jsonl \
  --pairs-db /caminho/pares_aeroportos.db
```

O replay preserva os horários históricos e bloqueia comandos. `--udp` usa o
`gs_cli` compilado em `build/`. A opção `--adsb` também pode ser passada diretamente
a `gs_web.py`; sem ela, o painel de telemetria existente continua disponível.

Testes: `python3 -m unittest test_adsb_pipeline test_gs_web test_tracks_to_ndjson`
a partir de `ground/host`. Incluem comando HTTP → processo simulado → registros →
painel, processamento com pares reais em base pequena, decisão combinada,
rejeições e falha de inicialização por ausência do banco. A validação física
Raspberry Pi + LoRa + ESP32 + USB permanece necessária.
