idf_component_register(SRCS "main.cpp"
                       EMBED_TXTFILES "index.html"
                       PRIV_REQUIRES asr_service esp_eth esp_netif esp_event esp_http_server esp_partition)
