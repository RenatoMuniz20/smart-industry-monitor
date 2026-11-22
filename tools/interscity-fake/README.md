# InterSCity falso

Resource Adaptor e Data Collector no formato da instância real, para
demonstração e testes de carga sem escrever na instância da UFMA. Só usa a
biblioteca padrão do Python.

    make -C tools/interscity-fake test      # 21 verificações
    scripts/fake-stack.sh up                # pipeline completo no Compose com o falso

Detalhes, opções de falha e latência: [docs/interscity-falso.md](../../docs/interscity-falso.md).
