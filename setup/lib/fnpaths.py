"""Folders for the setup's converters (set by fnskate_setup.py before they are imported)."""
import os
SD = os.environ.get('FNSKATE_DATA', '')           # <mod folder>\\skate-data
VGMSTREAM = os.environ.get('FNSKATE_VGMSTREAM', '')  # setup\\vgmstream\\vgmstream-cli.exe
