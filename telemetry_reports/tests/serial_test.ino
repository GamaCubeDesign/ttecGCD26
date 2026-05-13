void setup() {

    Serial.begin(115200);

    Serial.println("ESP32 pronta!");
}

void loop() {

    // Verifica se chegou algo pela serial
    if (Serial.available()) {

        // Lê a linha enviada
        String mensagem = Serial.readStringUntil('\n');

        // Remove espaços/quebra de linha
        mensagem.trim();

        // Envia de volta
        Serial.println(mensagem);
    }
}