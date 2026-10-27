# The samples/starter-mmo live tests' account when the environment ctest runs in has none: public test data for the
# TEST compose stack only (starter_mmo_account.ps1 registers and verifies it through Mailpit on first use). Set
# KAST_ONLINE_EMAIL / KAST_ONLINE_PASSWORD to use another account. Set here as environment variables, so the editor's
# --online-login-env and the account script read them as before: never on a command line.
if(NOT DEFINED ENV{KAST_ONLINE_EMAIL})
  set(ENV{KAST_ONLINE_EMAIL} "starter-live@example.com")
endif()
if(NOT DEFINED ENV{KAST_ONLINE_PASSWORD})
  set(ENV{KAST_ONLINE_PASSWORD} "starter-live-password-123")
endif()
