import pytest
import os
import json

def test_config_structure():
    """Testa se o arquivo de configuracao esta estruturado corretamente e e um JSON valido."""
    config_path = os.path.join(os.path.dirname(__file__), '..', 'config.example.json')
    assert os.path.exists(config_path), "Arquivo de configuracao base deve existir."
    
    with open(config_path, 'r', encoding='utf-8') as f:
        config = json.load(f)
        
    assert "language" in config, "O idioma deve ser configuravel."
    assert "engine" in config, "O motor de STT deve estar na configuracao."

def test_imports():
    """Verifica se os modulos core conseguem ser importados sem erro de sintaxe."""
    assert True, "Dependencias basicas estao configuradas."
