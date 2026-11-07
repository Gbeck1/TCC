# Alimentação e desconexões USB na Raspberry Pi 3B

Mensagens do kernel como `Undervoltage detected`, `Voltage normalised` e `USB disconnect` indicam que há subtensão na Raspberry e perda real de dispositivos USB. Trate a alimentação antes de atribuir essas ocorrências ao código. Se o teclado ou mouse USB também desaparece, a falha afeta o barramento da Pi.

A potência total de uma fonte ATX não garante que a tensão de 5 V chegue estável à Raspberry: cabos finos ou longos, conectores com mau contato, reguladores e a distribuição de alimentação podem provocar queda sob carga. Para a Pi 3B, use uma alimentação apropriada, tipicamente 5,1 V / 2,5 A, com cabo de boa qualidade. Considere também a tela e outros periféricos no consumo total; siga as especificações de alimentação da tela. Meça a linha de 5 V na própria Pi com o sistema funcionando se houver um multímetro disponível.

Comece verificando o cabo de alimentação e os conectores. Faça um ensaio com os 12 V das ventoinhas e do Peltier desligados, mantendo a Pi e o Mega alimentados corretamente, para observar se a perda USB persiste sem as cargas de potência. Verifique também o cabo USB de dados entre Pi e Mega.

Na sessão SSH local, o comando abaixo informa o estado de alimentação/limitação:

```bash
vcgencmd get_throttled
```

`0x0` é o resultado ideal. O bit 0 informa subtensão atual; o bit 16 registra que houve subtensão desde a partida. Flags históricas podem continuar presentes depois da troca do cabo até o próximo reinício e não provam que a tensão continua baixa naquele instante. Outras flags indicam limitação de frequência ou temperatura e devem ser interpretadas separadamente.

Para acompanhar somente as novas mensagens enquanto testa:

```bash
sudo dmesg --follow --human
```

Interrompa com Ctrl+C. Depois de estabilizar a alimentação, confira que não surgem novos avisos de subtensão ou novas desconexões USB durante o funcionamento. Feche cópias extras da aplicação e monitores seriais. A versão 0.5.0 pede acesso exclusivo à porta serial no Linux para impedir duas cópias atualizadas de compartilhá-la; isso não impede falhas elétricas nem controla programas que ignorem esse bloqueio cooperativo.

A aplicação mantém a porta aberta quando falta apenas telemetria recente; não fecha/reabre a USB por esse motivo. A ausência de dados é sinalizada como histórico. Reconexões após perda física não reenviam comandos de atuação anteriores. Não há watchdog ou desligamento adicional das cargas por timeout.

Estabilize a alimentação antes de gravar o Mega ou salvar a EEPROM. A perda de energia durante uma gravação pode interromper o processo; a verificação de CRC impede usar uma configuração parcialmente gravada, mas não recupera os valores anteriores.
