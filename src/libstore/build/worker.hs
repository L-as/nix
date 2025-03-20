module Worker where

import Foreign.StablePtr (StablePtr, newStablePtr)

foreign export ccall "nix_libstore_init_worker_state" nix_libstore_init_worker_state :: IO (StablePtr ())

init :: ()
init = ()

nix_libstore_init_worker_state = newStablePtr ()
