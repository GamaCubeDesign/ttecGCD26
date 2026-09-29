# Ground de rastreio de aeronaves

Aplicação Python com biblioteca padrão, usando o cadastro global OurAirports já importado. Estima origem e destino pelo método de aeroportos, com geometria de pistas, fase de voo e memória temporal. O método de rotas cadastradas foi removido.

## Atualização: viabilidade de descida em LEVEL

O ranking usa peso 0,15 para a tendência em `LEVEL` (antes 0,60). As outras fases mantêm seus pesos. Para destinos com altitude, elevação e velocidade positiva disponíveis, registra `required_descent_fpm` e `descent_feasibility`: a razão necessária é a altura relativa dividida pelo tempo até o aeroporto em distância direta e velocidade constante. A viabilidade vale 1 até 2.500 ft/min, 0 a partir de 5.000 ft/min e varia linearmente entre esses limiares. Esses números são heurísticos iniciais, não limites universais de aeronaves. Dados ausentes ou velocidade zero não geram uma viabilidade inventada.

A nota é multiplicada pela viabilidade em `LEVEL` **sem contexto recente de aproximação**. A exceção terminal foi preservada: a aplicação literal em todo LEVEL fez SBBR cair de 0,6878 para 0,0331 no teste E480F9, porque a distância direta não representa o percurso restante de uma aproximação com curvas. Os campos de diagnóstico continuam presentes nesse caso, mas o fator não é aplicado. A fusão de decisões e a interface não foram alteradas. Essa mudança modifica novas estimativas; o histórico salvo não é recalculado automaticamente.

## Atualização: contexto de aproximação e candidatos

O destino agora combina o movimento recente com até dez minutos observados. Altitude e velocidade são analisadas como séries separadas, pois podem chegar em mensagens sem posição. Uma queda acumulada de pelo menos 1.000 pés e tendência de descida sustentam um contexto de aproximação durante um trecho breve nivelado. Uma recuperação de altitude superior a 1.000 pés interrompe esse contexto. Isso é um indicador de comportamento, não uma confirmação de pouso.

Durante esse contexto, os componentes são: proximidade (peso 0,45, escala 25 km), convergência acumulada (0,30), altura relativa (0,15) e desaceleração (0,10, saturação em 80 nós). Componentes ausentes não viram zero. O afastamento de até 5 km da menor distância observada é tolerado; afastamentos maiores reduzem a convergência, com escala de 20 km. A pista acrescenta evidência positiva quando compatível com aproximação; desalinhamento fora da aproximação final não penaliza o candidato. Os pesos e limiares continuam heurísticos e não calibrados.

Todos os aeroportos dentro do raio participam da memória; `--top` limita apenas os candidatos apresentados no snapshot. A fusão exige persistência individual. Além de `ESTIMATED`/`UNKNOWN`, há `TENTATIVE`: candidato com evidência de aproximação, score ≥0,55 e distância ≤50 km, mas sem satisfazer todos os critérios de decisão. O terminal escreve **SBBR (candidato)**, por exemplo, com confiança baixa e decisão inconclusiva. Isso não equivale a aeroporto confirmado nem porcentagem de acerto. O JSON distingue `best_candidate`, `candidate`, `status`, `margin` e `ranking`.

A classificação instantânea LEVEL deixa de bloquear destino quando o contexto temporal de descida permanece válido. Sem esse contexto, continuam as regras conservadoras descritas abaixo. A origem continua exigindo evidência do trecho inicial de subida; não se inventa origem invertendo a direção de uma chegada.

O caso E480F9 foi preservado em `tests/fixtures/e480f9.ndjson`, com origem/destino informados pelo usuário usados apenas para validação. O teste em `tests/test_destination_context.py` verifica SBBR como candidato mesmo no estado LEVEL e não fornece o destino ao estimador. Não há ICAO nem aeroporto específico como regra no código de produção. O teste ainda não foi executado pelo assistente; não há ganho de acurácia medido nesta atualização.

## Fluxo simples: simulador + interface web

No primeiro terminal:

```bash
python simular_trajetorias.py
```

No segundo terminal:

```bash
python -m ground.visualize --serve
```

Abra o endereço impresso pelo segundo comando. Ele acompanha automaticamente `~/adsb/eventos.ndjson`, processa as mensagens e serve a interface; não é necessário executar `tail` ou `python -m ground` separadamente. Para outro arquivo, use `--input CAMINHO`. Não mantenha o consumidor manual anterior ativo ao mesmo tempo. Para continuar usando um processador externo, adicione `--no-ingest`.

**Ao vivo** é o modo inicial do servidor: usa a hora atual e mostra somente posições de até 60 segundos, sem aeronaves explicitamente no solo. Ausência de mensagens não confirma pouso: a aeronave apenas perde validade e some. A identificação de voo continua dependendo da telemetria; a tela não garante que todas as aeronaves visíveis estejam efetivamente voando. Use o simulador na velocidade padrão 1× nesse modo; dados acelerados com timestamps futuros devem ser examinados no histórico.

**Histórico** abre a lista de sessões salvas, os períodos e os controles de reprodução. **Voltar ao vivo** retorna à recepção atual. Os JSON e o SQLite preservam os registros anteriores, mas isso não obriga a exibi-los no mapa ao vivo. **Visão geral** fecha os detalhes mantendo o modo atual.

O acompanhamento lê linhas completas, aguarda arquivos ainda inexistentes e acompanha substituição/truncamento do arquivo. Na inicialização, recupera até 15 minutos de mensagens recentes para reconstruir o contexto; registros mais antigos do arquivo não são reprocessados como recepção ao vivo. Sessões já processadas permanecem no histórico. O processamento automático e o processador separado usam o método de aeroportos.

## Executar no VS Code

Abra esta pasta com **Arquivo → Abrir Pasta**, ou abra `ground-aeronaves.code-workspace`.
No terminal integrado, com esta pasta como diretório atual:

```bash
source .venv/bin/activate
python -m ground --input /home/pedro/adsb/eventos.ndjson
```

Requer Python 3.11 ou superior. O ambiente virtual `.venv` já foi criado sem pip, pois este MVP usa somente a biblioteca padrão. Não há pacotes externos para instalar. O arquivo informado pelo usuário foi localizado em `/home/pedro/adsb/eventos.ndjson` e não é alterado pela aplicação.

Por padrão, os resultados são gravados em `data/telemetria.db` e `data/estado.json`. Cada execução recebe um `session_id` independente. As observações anteriores são preservadas, mas não entram no cálculo da nova reprodução. Tabelas antigas recebem a coluna de sessão automaticamente; registros antigos não são apagados. Repetir o arquivo guarda uma nova sessão, sem usar timestamps futuros das sessões anteriores.

Também há uma configuração de depuração para dados reais e outra para a demonstração. O botão F5 requer a extensão Python Debugger do VS Code; os comandos no terminal funcionam sem essa extensão.

## Visualizar a trajetória reconstruída

Após processar a captura, gere o painel offline no terminal desta pasta:

```bash
python -m ground.visualize --serve
```

Abra o endereço exibido no terminal (normalmente **http://127.0.0.1:8766**; uma porta livre é escolhida se estiver ocupada) no navegador e mantenha o terminal aberto. Encerre com Ctrl+C. A tela mostra todas as aeronaves com posição válida no horário global selecionado, com o painel de dados oculto. Clique no ícone de uma aeronave (ou na sua trajetória) para destacá-la, centralizar suavemente o mapa e abrir os detalhes. **✕** fecha o painel; **Visão geral** fecha e reenquadra o conjunto. Ícones também podem ser selecionados com Tab e Enter. No painel, o seletor ICAO permite consultar inclusive aeronaves sem posição.

Use **Reproduzir** para avançar todas as aeronaves no mesmo relógio. Arrastar a barra pausa a reprodução e desativa o acompanhamento de novas mensagens. Os botões **−10 s / +10 s** permitem navegação fina. **Período da captura** separa trechos com intervalo superior a 15 minutos, evitando uma barra que abrange vários dias. **Pular intervalos sem aeronaves** só age durante a reprodução automática. Selecionar ou fechar uma aeronave não altera o tempo global. Somente posições já disponíveis naquele instante são mostradas; após 60 segundos sem posição, a aeronave sai do mapa. O rastro exibido cobre até 15 minutos anteriores. O mapa-base OpenStreetMap mostra ruas, cidades e contexto geográfico e precisa de internet. Desmarque **Mapa-base** para usar a grade e os dados locais offline. O provedor recebe pedidos de imagens da região visualizada; o arquivo ADS-B e os identificadores das aeronaves não são enviados. A atribuição fica visível no painel. O gráfico de altitude usa os valores recebidos/recentes nos estados salvos, pois altitude e posição podem chegar em mensagens separadas.

O painel reúne as posições dos snapshots armazenados pelo próprio estimador, sem recalcular hipóteses nem modificar o banco. As linhas apenas ligam amostras observadas; lacunas acima de 60 segundos, conflitos temporais e saltos incompatíveis interrompem a linha. O histórico continua disponível na reprodução, mas uma aeronave sem posição válida no instante selecionado não aparece no mapa. Aeronaves sem posições aparecem explicitamente sem trajetória. Os candidatos mostrados são os do instante selecionado, não uma confirmação dos aeroportos.

Para exportar apenas o arquivo offline, use `python -m ground.visualize` e abra `data/trajetorias.html`. Para mapa-base online, use `--serve`, pois o serviço exige a identificação normal de origem do navegador.

Por padrão é exportada a sessão com a estimativa mais recentemente gravada no SQLite. O HTML aberto diretamente é uma cópia estática. Com `--serve`, o painel consulta `/data` a cada 3 segundos e recebe as novas estimativas gravadas no SQLite. Ative **Acompanhar novas mensagens** para seguir o tempo da captura; sem novas mensagens o relógio continua avançando e posições antigas expiram. Para revisar o histórico, desative essa opção ou arraste a barra. O visualizador não substitui o processo de captura e processamento, que deve continuar rodando em outro terminal. Para selecionar outra sessão ou banco:

```bash
python -m ground.visualize --session ID_DA_SESSAO --telemetry-db data/telemetria.db --output data/trajetorias.html
```

## Arquitetura implementada

```text
Arquivo NDJSON / stdin
         ↓
Receiver (thread de leitura)
         ↓
Queue limitada a 256 mensagens
         ↓
Parser e validação ── inválidas → quarentena no SQLite
         ↓
AircraftManager → histórico original no SQLite
         ↓
Trajetória ordenada + valores recentes com proveniência
         ↓
Razão vertical robusta + qualidade da trajetória
         ↓
FlightStateEstimator (persistência e histerese)
         ↓
AirportEstimator ← cadastro global de aeroportos (somente leitura)
RouteMatcher     ← Playbook brasileiro (somente leitura; resultado separado)
         ↓
Fusão temporal limitada de hipóteses de aeroportos/pistas
         ↓
Histórico de estimativas + snapshot JSON
         ↓
API local de consulta (opcional, processo separado)
```

A recepção e o processamento usam threads e uma fila limitada. Essa implementação atende à leitura bloqueante de arquivo/stdin sem depender do loop de rede do asyncio. Ao atingir o limite, o produtor aguarda: mensagens da fila não são descartadas. Capacidade de recepção do rádio e buffers físicos ainda precisam ser medidos na integração.

| Arquivo | Responsabilidade |
|---|---|
| `ground/__main__.py` | CLI e coordenação dos fluxos |
| `ground/config.py` | Caminhos e parâmetros |
| `ground/receiver.py` | Leitura contínua até EOF e fila |
| `ground/parser.py` | Contrato NDJSON e validação |
| `ground/models.py` | Observação recebida |
| `ground/database.py` | Histórico e quarentena |
| `ground/aircraft_manager.py` | Processamento por ICAO e exportação |
| `ground/trajectory.py` | Posições ordenadas e campos recentes |
| `ground/flight_state.py` | Subida, cruzeiro, descida, solo ou desconhecido |
| `ground/geo.py` | Distância esférica, direção e diferença angular |
| `ground/airport_estimator.py` | Ranking geométrico de aeroportos |
| `ground/temporal.py` | Regressão vertical e qualidade da trajetória |
| `ground/runway.py` | Geometria das cabeceiras e aproximação |
| `ground/decision_fusion.py` | Memória de hipóteses, margem e abstenção |
| `ground/api.py` | API HTTP local somente leitura |

## Dados recebidos

Uma linha deve ser um objeto JSON com `icao` hexadecimal de seis caracteres. Campos opcionais: `rx_epoch_ns`, `lat`, `lon`, `altitude_ft`, `ground_speed_kt`, `track_deg`, `vertical_rate_fpm`, `on_ground` e quaisquer campos adicionais.

- A linha original é armazenada integralmente em `observations.raw_ndjson`.
- Campos ausentes permanecem ausentes. Zero não é confundido com ausência.
- `on_ground=-1`, observado no arquivo real, é preservado e interpretado como desconhecido; não herda um estado de solo anterior.
- Guardam-se o timestamp do embarcado e o recebimento na ground. Sem timestamp embarcado, usa-se o recebimento com `timestamp_source` explícito.
- Mensagens inválidas ficam em `rejected_messages`, com a linha original e o motivo.
- Mensagens fora de ordem entram no histórico sem fazer o estado atual voltar no tempo.
- A trajetória de processamento usa uma janela de 15 minutos, limitada a 2.000 observações. O histórico persistente continua completo.
- Últimos valores conhecidos expiram em 60 segundos de tempo dos dados; idade e proveniência aparecem no snapshot. Posição precisa de latitude e longitude na mesma observação.
- O histórico usado no processamento e o snapshot são restritos à execução atual. Retomar uma sessão anterior não está implementado.

## Como o estimador funciona (atualização de 23/09/2026)

O núcleo usa trajetória, fase do voo, aeroportos/pistas e memória de hipóteses. Não depende de haver uma rota comercial cadastrada. A linha AEROPORTOS inclui pistas e inferência temporal.

1. **Razão vertical:** preserva o valor recebido e calcula também uma regressão robusta Theil–Sen em até 30 altitudes dos últimos 90 segundos. Requer três altitudes e pelo menos dez segundos. A derivada é convertida de pés/segundo para pés/minuto. Resíduo mediano acima de 200 pés impede usar a derivada; diferença superior a 600 fpm gera um aviso de magnitude. Prefere regressão dos últimos 30 segundos e compara a razão recebida dos dez segundos mais recentes. Somente sentidos opostos acima de 300 fpm, com evidência recente comparável, bloqueiam a interpretação vertical. Isso não invalida a trajetória horizontal. Evidência vertical expira após 30 segundos.
2. **Qualidade:** exige três posições distintas no tempo, pelo menos 20 segundos de trajetória, lacunas de no máximo 60 segundos e ausência de saltos incompatíveis com o limite heurístico de 2 km + 0,65 km/s × intervalo. Posições conflitantes no mesmo instante também impedem decisão. Observações originais continuam preservadas.
3. **Fases:** `GROUND`, `CLIMB`, `LEVEL`, `DESCENT`, `APPROACH`, `UNKNOWN`. Subida/descida entram com razão superior a ±300 fpm e persistência de dez segundos em novas evidências. Histerese mantém a fase até ±150 fpm. Perda/conflito de dados permite UNKNOWN. GROUND exige indicação recebida. APPROACH exige descida, convergência e compatibilidade com a mesma cabeceira por dez segundos.
4. **Aeroportos candidatos:** busca espacial por latitude e distância esférica, raio padrão de 500 km (`--radius-km`), tipos pequeno/médio/grande. O ranking compara tendência robusta da distância nos últimos 120 segundos, altitude relativa, comportamento vertical, direção e pista. A direção instantânea tem peso pequeno e não é requisito obrigatório.
5. **Pistas:** usa as coordenadas das extremidades disponíveis, deslocamento de cabeceira, rumo verdadeiro (ou derivado das extremidades), distância transversal ao eixo, lado correto de aproximação/saída, distância à cabeceira e altura relativa. Sem elevação não declara aproximação. O gate inicial de aproximação exige 0–3.500 pés relativos, 0,3–20 km da cabeceira, até 1,5 km lateral e até 25° de diferença de direção. Ausência de pista não elimina o aeroporto.
6. **Memória:** média exponencial limitada com constante de 30 segundos, no máximo uma atualização por nova posição a cada dez segundos. Não multiplica o mesmo score repetidamente. Cada candidato vencedor precisa de três atualizações contínuas ao longo de pelo menos 20 segundos. Exige score ≥0,65, margem ≥0,08 sobre o segundo, cobertura ≥0,55, qualidade ≥0,35 e tendência favorável ≥0,65. Comparação mantém ao menos dois candidatos, inclusive com `--top 1`.
7. **Origem:** só recebe evidências novas durante subida nos primeiros 180 segundos observados. Requer altura inicial conhecida entre −300 e 6.000 pés relativos ao candidato. Depois preserva a hipótese aceita como histórica. **Destino:** exige DESCENT/APPROACH e altura relativa entre −300 e 10.000 pés. Um voo só observado em nível alto pode continuar indeterminado.

Fora do contexto acumulado descrito acima, pesos de compatibilidade em subida/descida: distância 0,15; direção 0,05; tendência 0,40; vertical 0,25; pista 0,15. Em APPROACH, pista recebe 0,50; em LEVEL/UNKNOWN, tendência recebe 0,60 e pista não contribui. Pesos disponíveis são renormalizados; cobertura é registrada. O componente vertical usa uma faixa ampla de plausibilidade, sem projetar uma descida constante até o chão.

**Todos esses números são parâmetros iniciais, ainda não calibrados.** A memória é uma fusão temporal de compatibilidade, não uma probabilidade bayesiana de acerto. Fase do voo funciona como condição de interpretação; não é multiplicada novamente como evidência independente da razão vertical.

Hipóteses antigas são marcadas como históricas; ambiguidade atual aparece como indeterminado. Um intervalo sem observações maior que 15 minutos reinicia o episódio de rastreamento. Subida após GROUND observado também reinicia as hipóteses. Isso não substitui uma segmentação completa de voos: toque e arremetida, arremetida e lacunas longas exigem validação específica.

A base de pistas possui dados ausentes. Altitude ADS-B e elevação precisam de referência vertical compatível; não foi adicionada correção de pressão/QNH. Não foram implementados modelos de performance por aeronave, clustering ou aprendizado de máquina.

Para avaliar somente o núcleo novo:

```bash
python -m ground --input /home/pedro/adsb/eventos.ndjson
```

O snapshot inclui `vertical_estimate`, `data_quality` e `airport_method.origin/destination`, com estado, motivo, score de compatibilidade, margem e evidências.

## Bancos e resultados

- `../banco-aeronaves/referencias.db`: referência global existente, preservada.
- `data/eventos.db`: validação executada com o arquivo real do usuário.
- `data/eventos-estado.json`: último estado das aeronaves dessa validação.
- `data/eventos-resultados.ndjson`: rankings gerados ao longo da validação.
- `data/demo.db`: demonstração sintética, separada dos dados reais.

Tabelas de telemetria: `aircraft`, `observations`, `rejected_messages`, `estimates`. A observação é confirmada antes de calcular a estimativa; em caso de falha posterior, o bruto permanece disponível.

## API opcional

Em outro terminal:

```bash
python3 -m ground.api --state data/eventos-estado.json
```

Endereços: `http://127.0.0.1:8765/state` e `http://127.0.0.1:8765/health`.
A API lê snapshots completos publicados por substituição atômica. Calcula a idade do último recebimento: ACTIVE até 60 s, STALE até 300 s, LOST depois disso. Esses estados indicam silêncio de recepção, não comprovam saída do alcance do rádio. O tempo de recebimento de um replay é o momento da reprodução.

## Testar

```bash
python3 -m unittest discover -s tests -v
python3 -m ground --input examples/aproximacao.ndjson --telemetry-db data/demo-novo.db --output data/demo-novo.json
```

Os testes cobrem preservação do bruto, mensagens incompletas/inválidas, `on_ground=-1`, ordem temporal, separação por ICAO, expiração de dados, reinício, direção, elevação ausente, meridiano de 180° e ingestão completa com quarentena.

A execução HTTP ao vivo não foi validada neste ambiente, que restringiu os canais de rede locais. A integração física LoRa ainda depende da interface do receptor. No momento, ele pode alimentar o stdin com linhas NDJSON decodificadas; este programa não configura a porta serial nem decodifica pacotes de rádio.

## Resultado no terminal

Ao concluir, uma linha por aeronave, identificada como `AEROPORTOS`, no formato:

```text
ICAO | MÉTODO | DECOLAGEM | POUSO | CONFIABILIDADE
```

Decolagem e pouso são aeroportos estimados, não eventos confirmados. No método AEROPORTOS, a coluna decolagem usa hipóteses de origem durante subida; pouso usa hipóteses de destino durante descida/aproximação. Durante subida/cruzeiro, aeroportos à frente não são apresentados como hipótese de pouso no resumo. Os rankings geométricos completos continuam disponíveis no JSON/SQLite.

Sem evidência suficiente, aparece `indeterminado`. Hipóteses anteriores da mesma execução são mantidas e identificadas como históricas quando antigas. A confiabilidade é indicada separadamente para decolagem e pouso: `baixa` para hipóteses da heurística atual, `insuficiente` quando não há hipótese. Não há porcentagens, pois o método ainda não foi calibrado com voos rotulados.

Use `--verbose` para diagnóstico por mensagem. A saída padrão é o resumo ao fim da leitura, sem milhares de linhas JSON.

A alteração de sessões e apresentação foi preparada para execução pelo usuário; a aplicação e os testes não foram executados novamente pelo assistente.

## Remoção do método de rotas cadastradas — 27/09/2026

O comparador de rotas, suas opções de linha de comando e o banco `rotas-brasil.db` foram removidos. As opções `--routes-db`, `--no-routes`, `--strict-routes`, `--route-tolerance-km` e `--with-routes` (avaliação) não são mais utilizadas. Novas estimativas e a API do mapa não publicam diagnóstico de rotas.

`ground/trajectory.py` continua organizando as posições realmente recebidas por ADS-B. Essa sequência é necessária para o método de aeroportos e para o mapa; ela não é uma base de rotas cadastradas. O simulador permanece disponível. O cadastro de aeroportos/pistas e a telemetria gravada foram preservados. Relatórios e pacotes históricos podem descrever a arquitetura anterior.

## Validação da atualização temporal

Preparados testes de regressão vertical com outlier, conflito recebido/derivado, histerese, saltos de posição, geometria de pista, altitude ausente, ambiguidades, pacotes repetidos, persistência individual de candidatos e integração com descida sem razão vertical recebida. Conforme a preferência de executar localmente no VS Code, o assistente não executou a aplicação nem os testes desta alteração. Foi realizada apenas verificação sintática estática; não representa validação de precisão.

```bash
python -m unittest discover -s tests -v
```

A direção arquitetural recuperada está em `docs/decisoes-estimativa-2026-09-23.md`. Esse documento é histórico: a implementação desta seção atualiza parte das pendências nele registradas.

## Correção após análise da execução de 23/09

A análise dos dados gravados identificou extrapolação indevida de pequenos trechos de rotas para os aeroportos do voo, comparação vertical entre janelas muito diferentes e invalidação indevida da geometria horizontal por conflito vertical. Essas falhas foram corrigidas. Naquela versão, a correspondência local de rota passou a ser apenas diagnóstico; o método foi posteriormente removido em 27/09/2026.

Os motivos horizontais agora distinguem poucas posições, trecho curto, lacuna, salto e posições conflitantes. `airport_last_evidence` guarda a última análise com ranking, para que o fim da janela inicial não oculte uma ambiguidade anterior. Não foi afrouxada a margem de decisão para forçar um aeroporto.

Veja `docs/diagnostico-execucao-2026-09-23.md`. A inspeção usou SQLite em modo somente leitura e snapshots já produzidos pelo usuário; não executou replay nem testes.

### Recepção contínua

O painel lê as estimativas; a decodificação de rádio é feita pelo capturador/decodificador anterior ao NDJSON. O núcleo `python -m ground --input -` processa mensagens de uma entrada contínua. Um arquivo aberto com `--input arquivo.ndjson` encerra no fim do arquivo e não acompanha novas linhas sozinho. Se o capturador estiver acrescentando mensagens ao arquivo, é possível acompanhar apenas novas linhas com:

```bash
tail -n 0 -F /home/pedro/adsb/eventos.ndjson | python -m ground --input -
```

Em outro terminal, execute `python -m ground.visualize --serve` e ative **Acompanhar novas mensagens**. O painel acompanha o horário dos eventos ADS-B, inclusive numa reprodução de dados antigos; não deve ser confundido com a hora atual do computador.

### Simular a chegada de mensagens NDJSON

O arquivo `simular_trajetorias.py` acrescenta mensagens ao arquivo `~/adsb/eventos.ndjson`, sem apagar o conteúdo existente. São quatro trajetórias sintéticas na região de Brasília, com identificadores de teste F00001–F00004, campos de posição/altitude/velocidade e timestamps compatíveis com o parser. Não há transmissão de rádio, decodificação SDR nem reprodução de procedimentos reais. As posições são cinemáticas simplificadas, úteis para testar recepção e visualização, não para medir acurácia com voos reais.

```bash
python simular_trajetorias.py
```

A execução padrão dura dez minutos, com um lote por segundo e horários comuns às aeronaves. Duas começam imediatamente, a terceira aos 90 segundos e a quarta aos 180 segundos. Cada uma deixa de emitir ao fim do seu trecho; o painel a oculta após o prazo de validade da última posição. Ctrl+C interrompe sem apagar linhas já escritas.

Para um arquivo separado ou reprodução acelerada:

```bash
python simular_trajetorias.py --output data/eventos_simulados.ndjson --speed 10
```

Com velocidade acima de 1, os timestamps avançam em tempo simulado e podem ultrapassar a hora do computador. O painel usa o relógio dos eventos. No modo avançado `--no-ingest`, inicie o consumidor **antes** do simulador (o arquivo precisa existir; `touch` preserva seu conteúdo):

```bash
touch /home/pedro/adsb/eventos.ndjson
tail -n 0 -F /home/pedro/adsb/eventos.ndjson | python -m ground --input -
```

Em outro terminal execute o simulador. Depois inicie `python -m ground.visualize --serve` em um terceiro terminal e marque **Acompanhar novas mensagens**. O simulador somente escreve; o consumidor transforma essas linhas nas estimativas que o painel consulta. Evite executar o simulador e um capturador real escrevendo no mesmo arquivo simultaneamente.
