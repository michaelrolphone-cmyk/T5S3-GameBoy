# Shared Game Boy convergence

Preservation baseline: T5S3-GameBoy 1.3.25, commit 6e061d5b8b2a8a301bc0838fb4bcc98c84f33aad. Retain the original sealed application/core/UI sources, custom 960x540 logical canvas, native navigation, save-state fixes, ROM discovery and atomic storage behavior. No new ROM downloads.

Product baseline: published RiscRTE-Watch Bringup 233e43841483adf570ae833a88d9b6fc08d68f1a over published System Home 83afd19e5184eb511cc205299c83eb9cb5c05430 and Contexts aa8b601a91367d65c06028d8dddfa79048191268. Unpublished successor application work is not a delivery input.

Work in progress: adapt physical display/input ownership to the delivered canvas provider; make nonessential hardware/storage contracts optional with explicit unavailable states. Build one complete identical ELF, then qualify all three actual product stores and SD packages, exact readback and selected Runtime admission normally and under sanitizers. Existing X4 legacy app policy remains until actual lifecycle evidence supports any migration. No hardware qualification is asserted.

Publication must be gated to this repository, its exact owning pull request and work/shared-gameboy-20261103 branch after all required qualification. Existing standalone master/tag release workflows are not shared-app publication targets.
