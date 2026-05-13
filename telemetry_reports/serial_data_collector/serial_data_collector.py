import serial
import csv
import os
from datetime import datetime

PORTA = "/dev/ttyUSB0" 

BAUDRATE = 115200

SYSTEMS = {

    "thermal_control": {

        "path": "telemetry_reports/thermalControl/thermalControlData.csv",

        "columns": [
            "temp_interna",
            "temp_externa"
        ]
    },

    "battery": {

        "path": "telemetry_reports/battery/batteryData.csv",

        "columns": [
            "voltage",
            "current"
        ]
    },

    "gps": {

        "path": "telemetry_reports/gps/gpsData.csv",

        "columns": [
            "latitude",
            "longitude",
            "satellites"
        ]
    }
}



for system_name, config in SYSTEMS.items():

    caminho = config["path"]

    pasta = os.path.dirname(caminho)

    os.makedirs(pasta, exist_ok=True)

    if not os.path.isfile(caminho):

        with open(caminho, "w", newline="") as arquivo:

            writer = csv.writer(arquivo)

            writer.writerow(
                ["timestamp"] + config["columns"]
            )


ser = serial.Serial(PORTA, BAUDRATE, timeout=1)

print("Monitorando telemetria...")

while True:

    try:

        linha = ser.readline().decode("utf-8").strip()

        if not linha:
            continue

        print(linha)

        partes = linha.split(",")

        system_name = partes[0]

        # Verifica se sistema existe
        if system_name not in SYSTEMS:

            print("Sistema desconhecido.")
            continue

        config = SYSTEMS[system_name]

        caminho_csv = config["path"]

        colunas = config["columns"]

        dados = {}

        for item in partes[1:]:

            if "=" not in item:
                continue

            chave, valor = item.split("=", 1)

            dados[chave] = valor

        linha_csv = [

            datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        ]

        for coluna in colunas:

            linha_csv.append(
                dados.get(coluna, "")
            )

        with open(caminho_csv, "a", newline="") as arquivo:

            writer = csv.writer(arquivo)

            writer.writerow(linha_csv)

        print(f"Dados salvos em {caminho_csv}")

    except KeyboardInterrupt:

        print("\nPrograma encerrado.")
        break

    except Exception as erro:

        print(f"Erro: {erro}")