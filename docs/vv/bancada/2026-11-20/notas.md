# Bancada — 20/11/2026: a antena de 1090 MHz

Só a antena de ADS-B, no PC, sem a Pi e sem o satélite: a antena COTS e a da
equipe, cada uma no NESDR Nano 3, com `make sdr-check`
(`tools/bench/sdr_check.py`; como ler o veredito: `../README.md`, "Teste da
antena de 1090 MHz"). Os testes do [roteiro do dia](roteiro.md) não foram feitos.

## Sessão

- Versão: `adsb-etapa-a-29-gc1d6e03` (branch `feat/sdr-check`)
- Receptor: NESDR Nano 3 (RTL2832U + R820T) no USB do PC; `dump1090` da FlightAware
  v11 (pacote `dump1090-fa-git`), ganho 49,6 dB, Mode A/C ligado
- Antenas: a COTS e a da equipe (dipolo de 1090 MHz com balun LC), no mesmo SDR e
  no mesmo cabo, trocadas entre as medições
- Local: um container (as medições das 10:15 e 10:22) e, depois, ao ar livre ao
  lado dele. **A confirmar:** onde foi a medição COTS das 10:11, e se o local é a FGA.
  As distâncias abaixo supõem o receptor na FGA (−15,99, −48,04).
- Referência: as aeronaves que a OpenSky via a até 150 km no início de cada medição

## Medições (`antena/`)

| Hora | Antena | Onde | s | Ruído | Mensagens com CRC certo | Aeronaves com posição | Alcance | Na OpenSky, no ar a até 100 km | Veredito |
|---|---|---|---|---|---|---|---|---|---|
| 10:11 | COTS | a confirmar | 60 | −39,4 dBFS | 763 | 3 | 62 km | 4 (a mais perto a 13 km) | **APROVADA** |
| 10:15 | equipe | container | 120 | −41,7 dBFS | 0 | 0 | — | 6 (a 10 km) | REPROVADA |
| 10:22 | equipe | container | 30 | −42,6 dBFS | 0 | 0 | — | 7 (a 18 km) | REPROVADA |
| 10:26 | COTS | ao ar livre | 60 | −37,3 dBFS | 472 | 2 | 51 km | 3 | **APROVADA** |
| 10:28 | equipe | ao ar livre | 60 | −41,5 dBFS | 141 | 2 | 74 km | 2 | **APROVADA** |
| 10:32 | equipe | ao ar livre | 60 | −42,2 dBFS | 12 | 1 | 56 km | 2 | **APROVADA** |

`make sdr-compare` refaz a tabela a partir dos `resumo.json`.

## A comparação entre as antenas

As medições das 10:26 (COTS) e 10:28 (equipe) ouviram as mesmas duas aeronaves,
no mesmo lugar, com dois minutos de diferença. Cada leitura de RSSI do
`aircraft.json` (a cada 5 s, posição com até 5 s) foi corrigida pela distância da
aeronave naquele instante, `RSSI + 20·log10(d)`, e comparada pela mediana:

| Aeronave | COTS | Equipe | Diferença |
|---|---|---|---|
| TAM4738 (`e486c1`) | 10 leituras, 42–51 km | 2 leituras, 73–74 km | −4,1 dB |
| TAM3432 (`e490f0`) | 11 leituras, 13–16 km | 10 leituras, 18–25 km | −5,4 dB |

**A antena da equipe entrega cerca de 4 a 5 dB a menos de sinal que a COTS.** O
ruído também cai cerca de 4 dB (−41,5 contra −37,3 dBFS), e ela recebeu um terço das
mensagens (141 contra 472). É uma estimativa: poucas leituras (2 da TAM4738), o
diagrama da antena do próprio avião muda com a atitude dele, e as distâncias supõem
o receptor na FGA.

## Achados

1. **O container bloqueia o sinal.** Lá dentro, a antena da equipe não recebeu
   nenhuma mensagem válida em 150 s, com 6 a 7 aeronaves no ar a até 100 km; ao ar
   livre, ao lado, recebeu. Um container metálico é uma gaiola de Faraday. É
   coerente com 16/11, em que a antena COTS dentro da sala também não recebeu nada
   (`../2026-11-16/notas.md`, etapa 1). Testes de antena: sempre ao ar livre.
2. **A antena da equipe funciona, ~4–5 dB abaixo da COTS.** Ouviu aeronave real a
   74 km. Para a competição, com o simulador da organização a poucos metros, a
   margem deve sobrar; ainda assim vale a equipe de antenas medir a sintonia (VNA ou
   medidor de ROE em 1090 MHz) e conferir o comprimento dos braços (meia onda: ~6,5 a
   6,9 cm cada), o balun e a solda.
3. **A referência da OpenSky está minutos atrasada.** Às 10:26 e às 10:28 ela
   devolveu a TAM4738 na mesma posição (33,0 km, 12 001 ft); às 10:32 punha a TAM3432
   a 3 775 ft, que ela já tinha passado às 10:28 (o nosso receptor a viu subindo,
   10 700 → 12 175 ft entre 10:28:51 e 10:29:41, e a 18 100 ft às 10:32). A consulta
   anônima serve para saber se havia aviões perto, que é o uso do `sdr_check`, mas não
   para comparar posições.
4. **Preâmbulos não são prova.** No container, a antena da equipe somou 425 618
   preâmbulos Mode S em 30 s, sem nenhuma mensagem com CRC certo: com ganho máximo,
   é o ruído do próprio receptor. O `sdr_check` já não os conta no veredito.

## Pendências

- Confirmar o local da medição COTS das 10:11 e a posição real do receptor; com ela,
  refazer as distâncias (`SDR_OPTS="--lat … --lon …"` nas próximas).
- Medir a sintonia da antena da equipe em 1090 MHz (VNA ou ROE).
- Com a antena aprovada ao ar livre: o teste 2 do [roteiro](roteiro.md), o
  `dump1090-fa` real com a porta AVR, agora pode ser feito na Pi — ao ar livre.
