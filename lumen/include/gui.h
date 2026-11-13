#ifndef GUI_H
#define GUI_H

/*
 * Interface graphique native Windows (Win32 API)
 * Compile en 64-bit (x64). Aucune virtualisation.
 * Retourne 0 en cas de succès, -1 en cas d'erreur.
 */
int gui_run(void);

#endif /* GUI_H */
