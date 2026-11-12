# Ground ADS-B — aeroportos e pares em paralelo

Esta é a versão integrada do projeto neste workspace. Mantém o estimador local de origem/destino e acrescenta a análise de pares direcionais com **conclusão combinada por fase de voo**. Os rankings de cada método continuam visíveis e independentes. A conclusão é heurística, não uma probabilidade calibrada.

## Conclusão combinada

O painel mostra os candidatos do método original, os pares geométricos e a conclusão atual. A fusão usa exclusivamente resultados da mesma versão da trilha e é persistida junto ao resultado assíncrono em `pair_results`. Resultados anteriores aparecem identificados pela versão e fase usada; o painel expira a conclusão após 60 s sem uma nova análise no tempo dos dados.

Origem e destino são avaliados separadamente. Na subida, utiliza-se o ranking local de origem; na descida/aproximação, o de destino. Uma origem anteriormente aceita pelo método local pode continuar como evidência histórica dentro do mesmo episódio. Os pesos locais/pares são 80/20 quando a extremidade relevante está próxima (até 50 km e altura relativa entre −300 e 6.000 ft para origem ou 10.000 ft para destino), 60/40 nas outras subidas/descidas, e 30/70 no voo nivelado com alinhamento geométrico de pelo menos 0,95. Se um método não oferece evidência para aquela extremidade, usa-se apenas o disponível. Curvas em voo nivelado impedem uma nova conclusão geométrica.

Ambas as notas entram como suporte limitado a [0,1], sem normalização pelo candidato vencedor e sem interpretação probabilística; a compatibilidade temporal local tem preferência sobre a nota instantânea. Essa é uma escala operacional inicial, ainda sem calibração empírica entre os métodos. Para cada aeroporto, usa-se o máximo suporte dos pares, nunca a soma da quantidade de rotas. Concordância no primeiro candidato da mesma extremidade acrescenta no máximo 0,05, por se tratar de evidência correlacionada.

Uma conclusão exige suporte mínimo 0,60, margem de 0,08, três análises com posições distintas e pelo menos 20 s de persistência. Trocar uma hipótese exige vantagem de 0,12 sobre a anterior e nova persistência. Lacunas maiores que 60 s, mudança de episódio/categoria, conflito vertical e dados inutilizáveis invalidam a memória correspondente. Uma extremidade ambígua fica indefinida; só se apresenta uma rota completa quando as duas extremidades são distintas e o par também consta entre os candidatos geométricos. Falhas do estimador de pares permitem conclusão parcial pelo método local. Com `--no-pairs`, o método local continua alimentando a conclusão parcial.

Os limites e pesos precisam de avaliação com voos de origem/destino conhecidos. O sistema não declara uma resposta definitiva quando os candidatos permanecem próximos.

## Executar

Na pasta `ground/host/ground-aeronaves`:

```bash
python3 -m ground.visualize --serve --input /home/pedro/adsb/eventos.ndjson
```

O painel acompanha novas linhas do arquivo, processa ambas as estratégias e normalmente abre serviço em `http://127.0.0.1:8766`. Se a porta estiver ocupada, o terminal informa outra porta. Acesse a URL mostrada. Mensagens incompletas aguardam a quebra de linha; arquivos substituídos ou truncados são reabertos.

Também há um lançador que já entra na pasta correta:

```bash
ground/host/iniciar_painel.sh
```

Para reproduzir um arquivo até o fim:

```bash
python3 -m ground --input validation/replay_pares.ndjson
python3 -m ground.visualize --serve --no-ingest
```

No painel, use **Histórico** para dados antigos. O modo ao vivo usa o relógio atual e oculta posições expiradas. Para acompanhar um processador que já esteja rodando, use `--no-ingest` e o mesmo `--telemetry-db`, evitando iniciar dois consumidores.

Python 3.10+ e NumPy são necessários para a análise de pares; esta integração foi validada com Python 3.14. Dependências estão em `requirements.txt`. Shapely só é necessário para reconstruir a prioridade geográfica do banco, não para processar voos. Se NumPy ou o banco de pares estiverem indisponíveis, o erro aparece na comparação e o método atual continua processando.

## Bancos

- `../banco-aeronaves/referencias.db`: cópia do cadastro original, usada pelo estimador atual em modo somente leitura.
- `../pares/pares_aeroportos.db`: banco mundial de pares e prioridades regionais, aberto em modo somente leitura por processo.
- `data/telemetria.db`: mensagens, estimativas atuais e resultados assíncronos de pares.
- `data/estado.json`: estado mais recente, publicado por substituição atômica.

As opções `--reference-db`, `--pairs-db`, `--telemetry-db` e `--output` permitem outros caminhos. As antigas opções `--routes-db` e `--strict-routes` não fazem parte desta implementação.

## Fluxo e concorrência

1. Uma thread recebe as linhas e coloca as mensagens em uma fila limitada. Se a fila encher, aplica espera ao produtor; não descarta deliberadamente observações.
2. O consumidor valida, grava as observações e executa o estimador atual, mantendo seu comportamento anterior.
3. Havendo qualidade suficiente, copia a mesma trilha e o resumo do resultado local para uma tarefa de pares.
4. Um conjunto fixo de processos executa o cálculo geométrico. Existe no máximo uma tarefa ativa por aeronave; novas mensagens substituem apenas a tarefa pendente daquela aeronave pela versão mais recente.
5. O consumidor coleta resultados também durante períodos sem mensagens. Publica os pares junto à identificação da versão usada, combinando-os apenas com a avaliação local da mesma versão, sem usar evidências mais novas.

A fila de tarefas guarda até 128 aeronaves pendentes. Ao atingir esse limite, a nova solicitação de análise fica adiada e é tentada na próxima observação; a observação e o método atual continuam sendo processados. Fim do replay drena as últimas tarefas, com limite de 30 s. A parada sinaliza análises que ficaram pendentes. Em Python 3.14, processos ainda ativos no limite são encerrados; em versões anteriores, o desligamento do interpretador pode aguardar uma tarefa ativa terminar.

Padrões e ajustes disponíveis nos dois comandos:

```bash
python3 -m ground.visualize --serve --pair-workers 2 --pair-interval 2 --pair-corridor-km 80
python3 -m ground.visualize --serve --no-pairs
```

O padrão é **1 processo**, intervalo mínimo de **1 s por aeronave**, corredor de **80 km**. Intervalo mínimo não é promessa de resultado a cada segundo: o cálculo pode demorar mais, e outras aeronaves compartilham os processos. Há limite de 1 a 4 processos; cada um mantém seu próprio cache. A API Python `Config()` mantém `pairs_enabled=False` por compatibilidade; os comandos habilitam a comparação por padrão.

## Geometria e ranking

O banco é percorrido na ordem regional alta → intermediária → normal, sem limitar a busca à América Latina. A classificação regional não acrescenta pontos. Cada par não direcionado é examinado uma vez; o avanço observado determina o sentido, evitando calcular duas vezes a mesma geometria.

- Usa o arco **finito** de grande círculo: um avião alinhado com o prolongamento da rota, mas longe dos dois extremos, não ganha nota por estar na mesma circunferência.
- Usa o trecho recente contínuo; lacunas maiores que 60 s, posições conflitantes e saltos reiniciam esse trecho.
- Exige pelo menos três posições, 20 s de observação e 2 km de deslocamento.
- A trajetória é amostrada em até 32 posições distribuídas no trecho, preservando seus extremos.
- A nota combina desvio RMS em relação ao arco, avanço no sentido da rota, monotonicidade e compatibilidade de categoria com os aeroportos.
- Considera no máximo 0,5 km de regressão entre amostras como tolerância; exige pelo menos 80% dos passos sem regressão maior e 1 km de avanço ao longo do arco.
- Retorna os melhores pares com nota heurística e confiança baixa. O número de pares examinados e o tempo de cálculo ficam registrados.

Os valores são parâmetros iniciais para testes. O corredor não representa faixa de navegação autorizada. O modelo esférico não representa aerovias, procedimentos de saída/chegada, alcance de aeronaves, escalas ou existência de voos comerciais. Trajetórias curtas podem produzir rotas distantes muito semelhantes, inclusive intercontinentais. Altitude e fase permanecem avaliadas pelo método atual; ainda não há uma segunda pontuação vertical específica dos pares.

## Versões e painel

Cada observação recebe `analysis_version`; desconexões longas e novas decolagens mudam `tracking_episode`. Os resultados dos pares incluem `source_version`, `source_event_ns`, `local_at_analysis`, `completed_received_ns` e `available_event_ns`. Um resultado antigo pode ser mostrado explicitamente como anterior, junto à avaliação local daquela mesma rodada. Resultados de outro episódio não substituem o acompanhamento atual.

A tabela `pair_results` guarda as conclusões assíncronas separadamente, referenciando a observação de origem. O painel só apresenta o resultado a partir do momento lógico em que ficou disponível no processamento; não o injeta em quadros anteriores da reprodução. A revisão do cache do painel muda quando um cálculo termina, mesmo sem nova mensagem. O quadro de pares mostra origem→destino, nota, desvio RMS e a versão; erros não removem as informações do método atual.

## Cobertura da base

O banco atual ainda contém os **3.291 aeroportos e 10.827.390 pares** da seleção inicial. Os 89 aeroportos regulares latino-americanos identificados pela auditoria ainda não foram incorporados. Não há inferência de helipontos ausentes do banco; o método atual continua capaz de avaliar instalações fora da seleção de pares. A auditoria por cidade está em `../pares/auditoria_cidades/LEIA-ME.md`.


## Validação

```bash
python3 -W ignore::ResourceWarning -m unittest discover -s tests -v
node tests/test_viewer.cjs
OPENBLAS_NUM_THREADS=1 python3 validation/benchmark_pairs.py
```

O benchmark cria uma trilha sintética sobre o arco SBBR→SBGR, recebe 21 mensagens e percorre o banco mundial. A rota verdadeira aparece em primeiro lugar; isso valida a integração e a geometria desse exemplo, **não mede precisão em voos reais**. `validation/resultado_pares.json` registra os tempos e candidatos. No ensaio inicial, a última análise levou cerca de 2 s com o cache carregado; a execução completa, incluindo carregamento e duas rodadas, levou cerca de 13 s. O custo varia com trajetória, máquina, número de aeronaves e processos.

O simulador existente continua em `simular_trajetorias.py`; sua saída é NDJSON. Não execute simulador e capturador físico escrevendo no mesmo arquivo ao mesmo tempo. Os testes cobrem geometria, sentido inverso, antimeridiano, arco finito, amostras insuficientes, fila limitada, substituição de pedidos, erros independentes, versões, histórico e publicação durante recepção ociosa.
