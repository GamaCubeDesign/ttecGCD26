# Diagnóstico da execução informada pelo usuário

Fontes locais: data/estado.json e telemetria.db, sessão f56a1d0247084dab8d0d8965a5efbbf6. Consulta somente leitura; não é validação contra origem/destino verdadeiro.

## Achados

- 20 aeronaves na sessão; oito sem nenhuma posição: E49756, E486C1, E47E05, DEF103, E48C82, E494BB, E48394, E493D1. Não podem ter inferência geométrica de aeroporto baseada nestes dados.
- E4A669 tem 145 posições no conjunto, altitude 39.975–40.025 pés. No snapshot final, só duas posições na janela, separadas por 67 segundos. O motivo final descreve o fim da recepção, não todo o voo.
- E4A598 foi associado a SCEL→SBRF pelo avanço de somente 5,925 km, distância lateral média de 5,617 km, com aviso na fonte. O código apresentava indevidamente os aeroportos cadastrados da rota como estimativa do voo.
- As geometrias SCEL→SBRF e SBRF→SCEL usadas são trechos publicados e não têm extremos nos dois aeroportos. A coincidência local não permite identificar esses aeroportos como origem/destino da aeronave. A falta de outros candidatos no banco também não comprova unicidade real.
- E49608: recebido +2.752 fpm e derivado +2.001,93 fpm. Ambos indicam subida, mas a diferença de magnitude tornava toda a qualidade inutilizável, inclusive para o matcher horizontal.
- E480F9: recebido −64 fpm e média derivada de cerca de 90 segundos −699,10 fpm. Comparar escalas temporais diferentes pode confundir uma transição para voo nivelado com conflito.
- E49C01: 395 mensagens classificadas como CLIMB; 338 análises de origem registradas como AMBIGUO. O problema não foi apenas a expiração final da posição nem o limite de 180 segundos. Os scores de aeroportos próximos eram semelhantes.
- E49F5B: último ranking de destino teve margem 0,0206, inferior a 0,08. Não há evidência neste diagnóstico para escolher um candidato verdadeiro ou reduzir essa margem.

## Correções realizadas

1. Matcher fornece CORRESPONDENCIA_PARCIAL e origin_destination_identified=false; terminal não transforma endpoints cadastrados em origem/destino inferidos. Candidatos ficam disponíveis no JSON para inspeção.
2. Razão vertical prefere regressão recente de 30 segundos e mediana recebida de dez segundos. Divergência de magnitude é aviso; conflito vertical exige sentidos opostos significativos com dados recentes comparáveis.
3. Qualidade horizontal não é anulada por conflito vertical. Motivos distinguem ausência de posição de insuficiência de trajetória.
4. Último ranking diagnóstico fica preservado em airport_last_evidence; o resumo da origem menciona a última análise ao encerrar a janela.

## Limites e validação pendente

O ranking de aeroportos ainda não foi calibrado e não discrimina adequadamente todos os aeródromos próximos. Esses ajustes corrigem erros de lógica e interpretação, mas não demonstram ganho de acurácia. Não há ground truth de origem/destino fornecido para essas aeronaves. Foram preparados testes de regressão; não foram executados testes nem aplicação, mantendo a execução com o usuário. Os arquivos de resultados existentes são anteriores às correções e foram preservados.
