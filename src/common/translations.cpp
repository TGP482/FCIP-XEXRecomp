// French, German, Italian and Spanish for the text the recomp adds, in the menus' capitals; SCDA's wording where it
// had the same setting
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "common/translations.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <unordered_map>

#include "app/app.h"

namespace fcip {

namespace {

// French, German, Italian, Spanish
struct Entry {
    std::string_view english;
    std::string_view text[4];
};

constexpr Entry kEntries[] = {
    // pages and sections
    {"RECOMP SETTINGS",
     {"PARAMÈTRES RECOMP", "RECOMP-EINSTELLUNGEN", "IMPOSTAZIONI RECOMP", "CONFIGURACIÓN DE RECOMP"}},
    {"MOUSE AND KEYBOARD", {"SOURIS ET CLAVIER", "MAUS UND TASTATUR", "MOUSE E TASTIERA", "RATÓN Y TECLADO"}},
    {"CONTROLLER", {"MANETTE", "CONTROLLER", "CONTROLLER", "MANDO"}},
    {"DEBUG", {"DÉBOGAGE", "DEBUG", "DEBUG", "DEPURACIÓN"}},
    {"DISPLAY", {"AFFICHAGE", "ANZEIGE", "VIDEO", "PANTALLA"}},
    {"GRAPHICS", {"GRAPHISMES", "GRAFIK", "GRAFICA", "GRÁFICOS"}},
    {"GENERAL", {"GÉNÉRAL", "ALLGEMEIN", "GENERALE", "GENERAL"}},
    {"INPUT DEVICES", {"PÉRIPHÉRIQUES", "EINGABEGERÄTE", "DISPOSITIVI DI INPUT", "DISPOSITIVOS DE ENTRADA"}},
    {"TEXTURES", {"TEXTURES", "TEXTUREN", "TEXTURE", "TEXTURAS"}},
    {"MOUSE", {"SOURIS", "MAUS", "MOUSE", "RATÓN"}},
    {"KEYBOARD", {"CLAVIER", "TASTATUR", "TASTIERA", "TECLADO"}},
    {"GYRO", {"GYROSCOPE", "GYROSKOP", "GIROSCOPIO", "GIROSCOPIO"}},
    {"CHEATS", {"CODES", "CHEATS", "TRUCCHI", "TRUCOS"}},
    {"LOAD LEVEL", {"CHARGER UN NIVEAU", "LEVEL LADEN", "CARICA LIVELLO", "CARGAR NIVEL"}},
    {"CUSTOM LEVELS", {"NIVEAUX PERSONNALISÉS", "EIGENE LEVELS", "LIVELLI PERSONALIZZATI", "NIVELES PERSONALIZADOS"}},
    {"SETTINGS", {"PARAMÈTRES", "EINSTELLUNGEN", "IMPOSTAZIONI", "CONFIGURACIÓN"}},
    {"RESTORE DEFAULTS",
     {"RÉTABLIR LES VALEURS PAR DÉFAUT", "STANDARD WIEDERHERSTELLEN", "RIPRISTINA PREDEFINITI",
      "RESTAURAR VALORES PREDETERMINADOS"}},

    // RECOMP SETTINGS rows
    {"MONITOR", {"ÉCRAN", "MONITOR", "MONITOR", "MONITOR"}},
    {"DISPLAY MODE", {"MODE D'AFFICHAGE", "ANZEIGEMODUS", "MODALITÀ SCHERMO", "MODO DE PANTALLA"}},
    {"RESOLUTION", {"RÉSOLUTION", "AUFLÖSUNG", "RISOLUZIONE", "RESOLUCIÓN"}},
    {"RENDER RESOLUTION",
     {"RÉSOLUTION DE RENDU", "RENDER-AUFLÖSUNG", "RISOLUZIONE DI RENDERING", "RESOLUCIÓN DE RENDERIZADO"}},
    {"FIELD OF VIEW", {"CHAMP DE VISION", "SICHTFELD", "CAMPO VISIVO", "CAMPO DE VISIÓN"}},
    {"VIEWMODEL FIELD OF VIEW",
     {"CHAMP DE VISION DE L'ARME", "SICHTFELD DER WAFFE", "CAMPO VISIVO DELL'ARMA", "CAMPO DE VISIÓN DEL ARMA"}},
    {"VERTICAL SYNC", {"SYNCHRO VERTICALE", "V-SYNC", "SINCRONIA VERTICALE", "SINCRONIZACIÓN VERTICAL"}},
    {"HDR OUTPUT", {"SORTIE HDR", "HDR-AUSGABE", "USCITA HDR", "SALIDA HDR"}},
    {"MAX FRAME RATE",
     {"IMAGES PAR SECONDE MAX.", "MAX. BILDRATE", "FRAME RATE MASSIMO", "FOTOGRAMAS POR SEGUNDO MÁX."}},
    {"FPS COUNTER", {"COMPTEUR D'IPS", "FPS-ANZEIGE", "CONTATORE FPS", "CONTADOR DE FPS"}},
    {"COLOUR BLIND MODE", {"MODE DALTONIEN", "FARBENBLINDMODUS", "MODALITÀ DALTONICI", "MODO DALTÓNICO"}},
    {"AMBIENT OCCLUSION", {"OCCLUSION AMBIANTE", "AMBIENT OCCLUSION", "OCCLUSIONE AMBIENTALE", "OCLUSIÓN AMBIENTAL"}},
    {"GLOBAL ILLUMINATION",
     {"ILLUMINATION GLOBALE", "GLOBALE BELEUCHTUNG", "ILLUMINAZIONE GLOBALE", "ILUMINACIÓN GLOBAL"}},
    {"VOLUMETRIC FOG", {"BROUILLARD VOLUMÉTRIQUE", "VOLUMETRISCHER NEBEL", "NEBBIA VOLUMETRICA", "NIEBLA VOLUMÉTRICA"}},
    {"VOLUMETRIC CLOUDS",
     {"NUAGES VOLUMÉTRIQUES", "VOLUMETRISCHE WOLKEN", "NUVOLE VOLUMETRICHE", "NUBES VOLUMÉTRICAS"}},
    {"CAUSTICS", {"CAUSTIQUES", "KAUSTIKEN", "CAUSTICHE", "CÁUSTICAS"}},
    {"OCEAN REFLECTION", {"REFLETS DE L'OCÉAN", "MEERESSPIEGELUNG", "RIFLESSI DELL'OCEANO", "REFLEJOS DEL OCÉANO"}},
    {"ANTI ALIASING", {"ANTICRÉNELAGE", "KANTENGLÄTTUNG", "ANTI-ALIASING", "ANTIALIASING"}},
    {"FRAME GENERATION",
     {"GÉNÉRATION D'IMAGES", "BILDGENERIERUNG", "GENERAZIONE DEI FOTOGRAMMI", "GENERACIÓN DE FOTOGRAMAS"}},
    {"ANISOTROPIC FILTERING",
     {"FILTRAGE ANISOTROPE", "ANISOTROPE FILTERUNG", "FILTRO ANISOTROPICO", "FILTRADO ANISOTRÓPICO"}},
    {"SHADOW RESOLUTION",
     {"RÉSOLUTION DES OMBRES", "SCHATTENAUFLÖSUNG", "RISOLUZIONE DELLE OMBRE", "RESOLUCIÓN DE SOMBRAS"}},
    {"SHADOW FILTERING", {"FILTRAGE DES OMBRES", "SCHATTENFILTERUNG", "FILTRO DELLE OMBRE", "FILTRADO DE SOMBRAS"}},
    {"DRAW DISTANCE", {"DISTANCE D'AFFICHAGE", "SICHTWEITE", "DISTANZA VISIVA", "DISTANCIA DE DIBUJADO"}},
    {"AIM AND SPRINT BLUR",
     {"FLOU DE VISÉE ET DE SPRINT", "UNSCHÄRFE BEIM ZIELEN UND SPRINTEN", "SFOCATURA DI MIRA E SCATTO",
      "DESENFOQUE AL APUNTAR Y AL CORRER"}},
    {"SKIP INTROS", {"PASSER LES INTROS", "INTROS ÜBERSPRINGEN", "SALTA INTRODUZIONE", "SALTAR INTRODUCCIÓN"}},
    {"PAUSE ON FOCUS LOSS",
     {"PAUSE EN ARRIÈRE-PLAN", "PAUSE IM HINTERGRUND", "PAUSA IN BACKGROUND", "PAUSA EN SEGUNDO PLANO"}},
    {"LANGUAGE", {"LANGUE", "SPRACHE", "LINGUA", "IDIOMA"}},
    {"AUDIO OUTPUT", {"SORTIE AUDIO", "AUDIOAUSGABE", "USCITA AUDIO", "SALIDA DE AUDIO"}},
    {"ONLINE JOIN IP",
     {"IP DE CONNEXION EN LIGNE", "ONLINE-BEITRITTS-IP", "IP PER UNIRSI ONLINE", "IP PARA UNIRSE EN LÍNEA"}},
    {"MICROPHONE", {"MICROPHONE", "MIKROFON", "MICROFONO", "MICRÓFONO"}},
    {"PUSH TO TALK", {"APPUYER POUR PARLER", "PUSH-TO-TALK", "PREMI PER PARLARE", "PULSAR PARA HABLAR"}},
    {"PLAYER", {"JOUEUR", "SPIELER", "GIOCATORE", "JUGADOR"}},
    {"TEXTURE REPLACEMENT",
     {"REMPLACEMENT DES TEXTURES", "TEXTURERSETZUNG", "SOSTITUZIONE DELLE TEXTURE", "REEMPLAZO DE TEXTURAS"}},
    {"TEXTURE DUMPING",
     {"EXTRACTION DES TEXTURES", "TEXTUREXTRAKTION", "ESTRAZIONE DELLE TEXTURE", "EXTRACCIÓN DE TEXTURAS"}},
    {"DUMP FORMAT", {"FORMAT D'EXTRACTION", "EXTRAKTIONSFORMAT", "FORMATO DI ESTRAZIONE", "FORMATO DE EXTRACCIÓN"}},
    {"INVINCIBILITY", {"INVINCIBILITÉ", "UNVERWUNDBARKEIT", "INVINCIBILITÀ", "INVENCIBILIDAD"}},

    // their values
    {"FULLSCREEN", {"PLEIN ÉCRAN", "VOLLBILD", "SCHERMO INTERO", "PANTALLA COMPLETA"}},
    {"BORDERLESS", {"FENÊTRÉ SANS BORDURE", "RANDLOSES FENSTER", "FINESTRA SENZA BORDI", "VENTANA SIN BORDES"}},
    {"WINDOWED", {"FENÊTRÉ", "FENSTER", "FINESTRA", "VENTANA"}},
    {"ON", {"OUI", "EIN", "SÌ", "SÍ"}},
    {"OFF", {"NON", "AUS", "NO", "NO"}},
    {"GAME DEFAULT", {"PAR DÉFAUT DU JEU", "SPIELSTANDARD", "PREDEFINITO DEL GIOCO", "PREDETERMINADO DEL JUEGO"}},
    {"(DEFAULT)", {"(PAR DÉFAUT)", "(STANDARD)", "(PREDEFINITO)", "(POR DEFECTO)"}},
    {"(RESTART)", {"(REDÉMARRAGE)", "(NEUSTART)", "(RIAVVIO)", "(REINICIO)"}},
    {"(NOT SUPPORTED)", {"(NON PRIS EN CHARGE)", "(NICHT UNTERSTÜTZT)", "(NON SUPPORTATO)", "(NO COMPATIBLE)"}},
    {"(MISSING FILES)", {"(FICHIERS MANQUANTS)", "(DATEIEN FEHLEN)", "(FILE MANCANTI)", "(FALTAN ARCHIVOS)"}},
    {"(PRIMARY)", {"(PRINCIPAL)", "(PRIMÄR)", "(PRINCIPALE)", "(PRINCIPAL)"}},
    {"REFRESH RATE",
     {"FRÉQUENCE DE RAFRAÎCHISSEMENT", "BILDWIEDERHOLFREQUENZ", "FREQUENZA DI AGGIORNAMENTO",
      "FRECUENCIA DE ACTUALIZACIÓN"}},
    {"UNLIMITED", {"ILLIMITÉ", "UNBEGRENZT", "ILLIMITATO", "ILIMITADO"}},
    {"AUTOMATIC", {"AUTOMATIQUE", "AUTOMATISCH", "AUTOMATICO", "AUTOMÁTICO"}},
    {"ENGLISH", {"ANGLAIS", "ENGLISCH", "INGLESE", "INGLÉS"}},
    {"FRENCH", {"FRANÇAIS", "FRANZÖSISCH", "FRANCESE", "FRANCÉS"}},
    {"GERMAN", {"ALLEMAND", "DEUTSCH", "TEDESCO", "ALEMÁN"}},
    {"ITALIAN", {"ITALIEN", "ITALIENISCH", "ITALIANO", "ITALIANO"}},
    {"SPANISH", {"ESPAGNOL", "SPANISCH", "SPAGNOLO", "ESPAÑOL"}},
    {"2.0 STEREO", {"2.0 STÉRÉO", "2.0 STEREO", "2.0 STEREO", "2.0 ESTÉREO"}},
    {"5.1 SURROUND", {"5.1 SURROUND", "5.1 SURROUND", "5.1 SURROUND", "5.1 ENVOLVENTE"}},
    {"PROTANOPIA", {"PROTANOPIE", "PROTANOPIE", "PROTANOPIA", "PROTANOPÍA"}},
    {"DEUTERANOPIA", {"DEUTÉRANOPIE", "DEUTERANOPIE", "DEUTERANOPIA", "DEUTERANOPÍA"}},
    {"TRITANOPIA", {"TRITANOPIE", "TRITANOPIE", "TRITANOPIA", "TRITANOPÍA"}},
    {"SHARP", {"NETTES", "SCHARF", "NITIDE", "NÍTIDAS"}},
    {"SOFT", {"DOUCES", "WEICH", "MORBIDE", "SUAVES"}},
    {"RAY TRACED", {"RAY TRACING", "RAYTRACING", "RAY TRACING", "TRAZADO DE RAYOS"}},
    {"ORIGINAL XBOX", {"XBOX ORIGINALE", "ORIGINAL-XBOX", "XBOX ORIGINALE", "XBOX ORIGINAL"}},
    {"ULTRA PERFORMANCE", {"ULTRA PERFORMANCES", "ULTRA-LEISTUNG", "ULTRA PRESTAZIONI", "ULTRARRENDIMIENTO"}},
    {"PERFORMANCE", {"PERFORMANCES", "LEISTUNG", "PRESTAZIONI", "RENDIMIENTO"}},
    {"BALANCED", {"ÉQUILIBRÉ", "AUSGEWOGEN", "BILANCIATO", "EQUILIBRADO"}},
    {"QUALITY", {"QUALITÉ", "QUALITÄT", "QUALITÀ", "CALIDAD"}},
    {"NATIVE AA", {"AA NATIF", "NATIVES AA", "AA NATIVO", "AA NATIVO"}},
    {"SYSTEM DEFAULT",
     {"PAR DÉFAUT DU SYSTÈME", "SYSTEMSTANDARD", "PREDEFINITO DI SISTEMA", "PREDETERMINADO DEL SISTEMA"}},
    {"(UNPLUGGED)", {"(DÉBRANCHÉ)", "(ENTFERNT)", "(SCOLLEGATO)", "(DESCONECTADO)"}},
    {"(FROM PLAYER", {"(DU JOUEUR", "(VON SPIELER", "(DAL GIOCATORE", "(DEL JUGADOR"}},
    {"NOT CONNECTED", {"NON CONNECTÉ", "NICHT VERBUNDEN", "NON COLLEGATO", "NO CONECTADO"}},
    {"YAW", {"LACET", "GIEREN", "IMBARDATA", "GUIÑADA"}},
    {"ROLL", {"ROULIS", "ROLLEN", "ROLLIO", "ALABEO"}},
    {"WORLD", {"MONDE", "WELT", "MONDO", "MUNDO"}},
    {"NONE", {"AUCUN", "KEINE", "NESSUNO", "NINGUNO"}},
    {"TOUCHPAD", {"PAVÉ TACTILE", "TOUCHPAD", "TOUCHPAD", "PANEL TÁCTIL"}},
    {"MISC", {"DIVERS", "SONSTIGE", "VARIE", "VARIOS"}},
    {"RIGHT PADDLE", {"PALETTE DROITE", "PADDLE RECHTS", "PALETTA DESTRA", "PALANCA DERECHA"}},
    {"LEFT PADDLE", {"PALETTE GAUCHE", "PADDLE LINKS", "PALETTA SINISTRA", "PALANCA IZQUIERDA"}},
    {"OFF WHILE HELD",
     {"DÉSACTIVÉ SI MAINTENU", "AUS BEIM HALTEN", "DISATTIVATO SE TENUTO PREMUTO", "DESACTIVADO AL MANTENER"}},
    {"ON WHILE HELD", {"ACTIVÉ SI MAINTENU", "EIN BEIM HALTEN", "ATTIVATO SE TENUTO PREMUTO", "ACTIVADO AL MANTENER"}},
    {"TOGGLE", {"BASCULER", "UMSCHALTEN", "ALTERNA", "ALTERNAR"}},
    {"START", {"DÉMARRER", "STARTEN", "AVVIA", "INICIAR"}},
    {"KEEP STILL", {"NE BOUGEZ PAS", "NICHT BEWEGEN", "NON MUOVERE", "NO LO MUEVAS"}},

    // MOUSE AND KEYBOARD and CONTROLLER rows
    {"HORIZONTAL SENSITIVITY",
     {"SENSIBILITÉ HORIZONTALE", "HORIZONTALE EMPFINDLICHKEIT", "SENSIBILITÀ ORIZZONTALE", "SENSIBILIDAD HORIZONTAL"}},
    {"VERTICAL SENSITIVITY",
     {"SENSIBILITÉ VERTICALE", "VERTIKALE EMPFINDLICHKEIT", "SENSIBILITÀ VERTICALE", "SENSIBILIDAD VERTICAL"}},
    {"AIM HORIZONTAL SENSITIVITY",
     {"SENSIBILITÉ HORIZONTALE EN VISÉE", "HORIZONTALE ZIELEMPFINDLICHKEIT", "SENSIBILITÀ ORIZZONTALE IN MIRA",
      "SENSIBILIDAD HORIZONTAL AL APUNTAR"}},
    {"AIM VERTICAL SENSITIVITY",
     {"SENSIBILITÉ VERTICALE EN VISÉE", "VERTIKALE ZIELEMPFINDLICHKEIT", "SENSIBILITÀ VERTICALE IN MIRA",
      "SENSIBILIDAD VERTICAL AL APUNTAR"}},
    {"INVERT X-AXIS", {"INVERSER L'AXE X", "X-ACHSE UMKEHREN", "INVERTI ASSE X", "INVERTIR EJE X"}},
    {"INVERT Y-AXIS", {"INVERSER L'AXE Y", "Y-ACHSE UMKEHREN", "INVERTI ASSE Y", "INVERTIR EJE Y"}},
    {"HOLD TO AIM", {"MAINTENIR POUR VISER", "HALTEN ZUM ZIELEN", "TIENI PREMUTO PER MIRARE", "MANTENER PARA APUNTAR"}},
    {"VIBRATION STRENGTH",
     {"INTENSITÉ DES VIBRATIONS", "VIBRATIONSSTÄRKE", "INTENSITÀ VIBRAZIONE", "INTENSIDAD DE LA VIBRACIÓN"}},
    {"HOLD TO SPRINT",
     {"MAINTENIR POUR SPRINTER", "HALTEN ZUM SPRINTEN", "TIENI PREMUTO PER SCATTARE", "MANTENER PARA CORRER"}},
    {"GYRO AIMING", {"VISÉE GYROSCOPIQUE", "GYRO-ZIELEN", "MIRA GIROSCOPICA", "APUNTADO CON GIROSCOPIO"}},
    {"GYRO SENSITIVITY",
     {"SENSIBILITÉ DU GYROSCOPE", "GYRO-EMPFINDLICHKEIT", "SENSIBILITÀ DEL GIROSCOPIO", "SENSIBILIDAD DEL GIROSCOPIO"}},
    {"GYRO AXIS", {"AXE DU GYROSCOPE", "GYRO-ACHSE", "ASSE DEL GIROSCOPIO", "EJE DEL GIROSCOPIO"}},
    {"GYRO BUTTON", {"BOUTON DU GYROSCOPE", "GYRO-TASTE", "PULSANTE DEL GIROSCOPIO", "BOTÓN DEL GIROSCOPIO"}},
    {"GYRO BUTTON MODE",
     {"MODE DU BOUTON DU GYROSCOPE", "GYRO-TASTENMODUS", "MODALITÀ PULSANTE DEL GIROSCOPIO",
      "MODO DEL BOTÓN DEL GIROSCOPIO"}},
    {"RECENTER BUTTON",
     {"BOUTON DE RECENTRAGE", "ZENTRIEREN-TASTE", "PULSANTE DI RICENTRATURA", "BOTÓN DE RECENTRADO"}},
    {"CALIBRATE GYRO",
     {"CALIBRER LE GYROSCOPE", "GYRO KALIBRIEREN", "CALIBRA IL GIROSCOPIO", "CALIBRAR EL GIROSCOPIO"}},
    {"PRESS A KEY", {"APPUYEZ SUR UNE TOUCHE", "TASTE DRÜCKEN", "PREMI UN TASTO", "PULSA UNA TECLA"}},
    {"UNBOUND", {"NON ASSIGNÉ", "NICHT BELEGT", "NON ASSEGNATO", "SIN ASIGNAR"}},

    // key binds
    {"MOVE FORWARD", {"AVANCER", "VORWÄRTS", "AVANTI", "AVANZAR"}},
    {"MOVE BACK", {"RECULER", "RÜCKWÄRTS", "INDIETRO", "RETROCEDER"}},
    {"STRAFE LEFT",
     {"PAS DE CÔTÉ À GAUCHE", "SEITWÄRTS LINKS", "SPOSTAMENTO LATERALE A SINISTRA", "DESPLAZAMIENTO A LA IZQUIERDA"}},
    {"STRAFE RIGHT",
     {"PAS DE CÔTÉ À DROITE", "SEITWÄRTS RECHTS", "SPOSTAMENTO LATERALE A DESTRA", "DESPLAZAMIENTO A LA DERECHA"}},
    {"WALK", {"MARCHER", "GEHEN", "CAMMINA", "CAMINAR"}},
    {"JUMP", {"SAUTER", "SPRINGEN", "SALTA", "SALTAR"}},
    {"CROUCH", {"S'ACCROUPIR", "DUCKEN", "ACCOVACCIATI", "AGACHARSE"}},
    {"FIRE", {"TIRER", "FEUERN", "FUOCO", "DISPARAR"}},
    {"AIM", {"VISER", "ZIELEN", "MIRA", "APUNTAR"}},
    {"MELEE", {"CORPS À CORPS", "NAHKAMPF", "CORPO A CORPO", "CUERPO A CUERPO"}},
    {"RELOAD", {"RECHARGER", "NACHLADEN", "RICARICA", "RECARGAR"}},
    {"INTERACT", {"INTERAGIR", "INTERAGIEREN", "INTERAGISCI", "INTERACTUAR"}},
    {"FERAL RUN", {"COURSE SAUVAGE", "WILDER LAUF", "CORSA FERINA", "CARRERA SALVAJE"}},
    {"FERAL VISION", {"VISION SAUVAGE", "WILDE SICHT", "VISTA FERINA", "VISIÓN SALVAJE"}},
    {"THROW", {"LANCER", "WERFEN", "LANCIA", "LANZAR"}},
    {"GRENADES / ROCKS", {"GRENADES / PIERRES", "GRANATEN / STEINE", "GRANATE / SASSI", "GRANADAS / PIEDRAS"}},
    {"WEAPONS / TRAPS", {"ARMES / PIÈGES", "WAFFEN / FALLEN", "ARMI / TRAPPOLE", "ARMAS / TRAMPAS"}},
    {"BINOCULARS", {"JUMELLES", "FERNGLAS", "BINOCOLO", "PRISMÁTICOS"}},
    {"PISTOL", {"PISTOLET", "PISTOLE", "PISTOLA", "PISTOLA"}},
    {"RIFLE", {"FUSIL", "GEWEHR", "FUCILE", "FUSIL"}},
    {"HEAVY WEAPON", {"ARME LOURDE", "SCHWERE WAFFE", "ARMA PESANTE", "ARMA PESADA"}},
    {"NEXT WEAPON", {"ARME SUIVANTE", "NÄCHSTE WAFFE", "ARMA SUCCESSIVA", "ARMA SIGUIENTE"}},
    {"PREVIOUS WEAPON", {"ARME PRÉCÉDENTE", "VORHERIGE WAFFE", "ARMA PRECEDENTE", "ARMA ANTERIOR"}},
    {"OBJECTIVES", {"OBJECTIFS", "ZIELE", "OBIETTIVI", "OBJETIVOS"}},
    {"TOGGLE HUD",
     {"AFFICHER / MASQUER L'INTERFACE", "HUD EIN / AUS", "MOSTRA / NASCONDI HUD", "MOSTRAR / OCULTAR HUD"}},
    {"TOGGLE TEXTURE REPLACEMENT",
     {"ACTIVER / DÉSACTIVER LE REMPLACEMENT DES TEXTURES", "TEXTURERSETZUNG EIN / AUS",
      "ATTIVA / DISATTIVA SOSTITUZIONE DELLE TEXTURE", "ACTIVAR / DESACTIVAR REEMPLAZO DE TEXTURAS"}},
    {"PAUSE MENU", {"MENU PAUSE", "PAUSENMENÜ", "MENU DI PAUSA", "MENÚ DE PAUSA"}},
    {"SETTINGS PANEL",
     {"PANNEAU DES PARAMÈTRES", "EINSTELLUNGSMENÜ", "PANNELLO IMPOSTAZIONI", "PANEL DE CONFIGURACIÓN"}},
    {"NOCLIP KEY", {"TOUCHE NOCLIP", "NOCLIP-TASTE", "TASTO NOCLIP", "TECLA NOCLIP"}},
    {"LOOK UP", {"REGARDER EN HAUT", "NACH OBEN SEHEN", "GUARDA IN ALTO", "MIRAR ARRIBA"}},
    {"LOOK DOWN", {"REGARDER EN BAS", "NACH UNTEN SEHEN", "GUARDA IN BASSO", "MIRAR ABAJO"}},
    {"LOOK LEFT", {"REGARDER À GAUCHE", "NACH LINKS SEHEN", "GUARDA A SINISTRA", "MIRAR A LA IZQUIERDA"}},
    {"LOOK RIGHT", {"REGARDER À DROITE", "NACH RECHTS SEHEN", "GUARDA A DESTRA", "MIRAR A LA DERECHA"}},

    // the F10 panel's own
    {"Back", {"Retour", "Zurück", "Indietro", "Atrás"}},
    {"Defaults", {"Par défaut", "Standard", "Predefiniti", "Por defecto"}},
    {"Apply", {"Appliquer", "Übernehmen", "Applica", "Aplicar"}},
    {"Yes", {"Oui", "Ja", "Sì", "Sí"}},
    {"No", {"Non", "Nein", "No", "No"}},
    {"Restore every setting on this page to its default?",
     {"Rétablir les valeurs par défaut de tous les paramètres de cette page ?",
      "Alle Einstellungen dieser Seite auf den Standard zurücksetzen?",
      "Ripristinare tutte le impostazioni di questa pagina ai valori predefiniti?",
      "¿Restaurar todos los ajustes de esta página a sus valores predeterminados?"}},
    {"Up and Down select, Left and Right change, Escape closes",
     {"Haut et Bas pour choisir, Gauche et Droite pour modifier, Échap pour fermer",
      "Auf und Ab wählen, Links und Rechts ändern, Esc schließt",
      "Su e Giù per scegliere, Sinistra e Destra per cambiare, Esc per chiudere",
      "Arriba y Abajo para elegir, Izquierda y Derecha para cambiar, Esc para cerrar"}},
    {"Press a key or mouse button to bind, Escape cancels",
     {"Appuyez sur une touche ou un bouton de souris, Échap pour annuler",
      "Taste oder Maustaste zum Belegen drücken, Esc bricht ab",
      "Premi un tasto o un pulsante del mouse da assegnare, Esc annulla",
      "Pulsa una tecla o un botón del ratón para asignarlo, Esc cancela"}},
    {"Enter or a click rebinds, right click or Delete clears the key",
     {"Entrée ou un clic pour réassigner, clic droit ou Suppr pour effacer la touche",
      "Eingabe oder Klick belegt neu, Rechtsklick oder Entf löscht die Taste",
      "Invio o un clic per riassegnare, clic destro o Canc per cancellare il tasto",
      "Intro o un clic para reasignar, clic derecho o Supr para borrar la tecla"}},
    {"Enter or a click loads the level, from the main menu or in game",
     {"Entrée ou un clic charge le niveau, depuis le menu principal ou en jeu",
      "Eingabe oder Klick lädt das Level, im Hauptmenü oder im Spiel",
      "Invio o un clic carica il livello, dal menu principale o in gioco",
      "Intro o un clic carga el nivel, desde el menú principal o en la partida"}},

    // the game's menus
    {"EXIT", {"QUITTER", "BEENDEN", "ESCI", "SALIR"}},
    {"CUSTOM CAMPAIGN",
     {"CAMPAGNE PERSONNALISÉE", "EIGENE KAMPAGNE", "CAMPAGNA PERSONALIZZATA", "CAMPAÑA PERSONALIZADA"}},
    {"ACHIEVEMENTS", {"SUCCÈS", "ERFOLGE", "OBIETTIVI", "LOGROS"}},
    {"CAMPAIGN", {"CAMPAGNE", "KAMPAGNE", "CAMPAGNA", "CAMPAÑA"}},
    {"MULTIPLAYER", {"MULTIJOUEUR", "MEHRSPIELER", "MULTIGIOCATORE", "MULTIJUGADOR"}},
    {"RESET ACHIEVEMENTS",
     {"RÉINITIALISER LES SUCCÈS", "ERFOLGE ZURÜCKSETZEN", "AZZERA GLI OBIETTIVI", "RESTABLECER LOGROS"}},
    {"Lock every achievement this profile has earned?",
     {"Verrouiller tous les succès obtenus par ce profil ?", "Alle von diesem Profil erzielten Erfolge sperren?",
      "Bloccare tutti gli obiettivi ottenuti da questo profilo?",
      "¿Bloquear todos los logros obtenidos por este perfil?"}},
    {"Are you sure you want to exit to the desktop?",
     {"Voulez-vous vraiment quitter et revenir au bureau ?",
      "Möchten Sie das Spiel wirklich beenden und zum Desktop zurückkehren?",
      "Vuoi davvero uscire e tornare al desktop?", "¿Seguro que quieres salir al escritorio?"}},
    {"Are you sure you want to launch the map editor?",
     {"Voulez-vous vraiment lancer l'éditeur de cartes ?", "Möchten Sie den Karteneditor wirklich starten?",
      "Vuoi davvero avviare l'editor di mappe?", "¿Seguro que quieres iniciar el editor de mapas?"}},
    {"ENTER THE HOST'S IP ADDRESS",
     {"SAISISSEZ L'ADRESSE IP DE L'HÔTE", "IP-ADRESSE DES HOSTS EINGEBEN", "INSERISCI L'INDIRIZZO IP DELL'HOST",
      "INTRODUCE LA DIRECCIÓN IP DEL ANFITRIÓN"}},
    {"Achievement unlocked:",
     {"Succès débloqué :", "Erfolg freigeschaltet:", "Obiettivo sbloccato:", "Logro desbloqueado:"}},
    {"CREATE A PROFILE", {"CRÉER UN PROFIL", "PROFIL ERSTELLEN", "CREA UN PROFILO", "CREAR UN PERFIL"}},
    {"A PROFILE WITH THIS NAME EXISTS",
     {"UN PROFIL PORTE DÉJÀ CE NOM", "EIN PROFIL MIT DIESEM NAMEN EXISTIERT BEREITS",
      "ESISTE GIÀ UN PROFILO CON QUESTO NOME",
      "YA EXISTE UN PERFIL CON ESTE NOMBRE"}},
    {"THIS NAME CANNOT BE USED",
     {"CE NOM NE PEUT PAS ÊTRE UTILISÉ", "DIESER NAME KANN NICHT VERWENDET WERDEN", "QUESTO NOME NON PUÒ ESSERE USATO",
      "ESTE NOMBRE NO SE PUEDE USAR"}},
};

// the entry's column for the language the game booted with, -1 for English
int Column() {
    switch (app::BootLanguage()) {
        case app::kFrench: return 0;
        case app::kGerman: return 1;
        case app::kItalian: return 2;
        case app::kSpanish: return 3;
        default: return -1;
    }
}

const std::unordered_map<std::string_view, const Entry*>& Entries() {
    static const auto entries = [] {
        std::unordered_map<std::string_view, const Entry*> map;
        for (const Entry& e : kEntries) map.emplace(e.english, &e);
        return map;
    }();
    return entries;
}

bool WordChar(char c) { return std::isalnum(uint8_t(c)) != 0; }

// the longest phrase with an entry from i, which must start a word; nullptr when none
const Entry* Phrase(std::string_view text, size_t i, size_t& end) {
    if (i && WordChar(text[i - 1])) return nullptr;
    for (end = text.size(); end > i; end--) {
        if (end < text.size() && WordChar(text[end])) continue;
        auto it = Entries().find(text.substr(i, end - i));
        if (it != Entries().end()) return it->second;
    }
    return nullptr;
}

}  // namespace

std::string Translate(std::string_view english) {
    static const int column = Column();
    if (column < 0) return std::string(english);
    std::string out;
    for (size_t i = 0; i < english.size();) {
        size_t end = i;
        const Entry* found = Phrase(english, i, end);
        if (!found) {
            out += english[i++];
            continue;
        }
        out += found->text[column];
        i = end;
    }
    return out;
}

std::u16string Utf16(std::string_view utf8) {
    int units = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::u16string out(size_t(units), u'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), reinterpret_cast<wchar_t*>(out.data()), units);
    return out;
}

}  // namespace fcip
