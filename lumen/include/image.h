#ifndef IMAGE_H
#define IMAGE_H

/*
 * Génère une image via le binaire stable-diffusion.cpp (compilé séparément
 * dans third_party/stable-diffusion.cpp/build/bin/sd).
 *
 * On passe par exécution du binaire plutôt que par liaison directe de la
 * bibliothèque : l'API interne de stable-diffusion.cpp change souvent d'une
 * version à l'autre, alors que son CLI est stable. C'est aussi le pattern
 * utilisé par la plupart des intégrations tierces du projet.
 *
 * sd_binary_path : chemin vers l'exécutable `sd`
 * model_path     : chemin vers le modèle .gguf de diffusion
 * prompt         : description textuelle de l'image
 * output_path    : chemin de sortie du fichier .png
 *
 * Retourne 0 en cas de succès, -1 en cas d'erreur.
 */
int image_generate(const char *sd_binary_path,
                    const char *model_path,
                    const char *prompt,
                    const char *output_path);

#endif /* IMAGE_H */
