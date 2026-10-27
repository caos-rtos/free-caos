# caOS FREE — End User License Agreement

Copyright © 2026 **Angelo Coppi**. All rights reserved.
caOS = **C**oppi **A**ngelo **O**perating **S**ystem.

This End User License Agreement ("Agreement") is a legal agreement between you ("Licensee")
and Angelo Coppi ("Licensor") for the caOS FREE edition, consisting of the precompiled
libraries, header files, examples, build files and documentation in this package (the
"Software").

By using the Software you accept this Agreement. If you do not accept it, do not use the
Software.

## 1. Licence granted

The Licensor grants you a free of charge, non-exclusive, non-transferable licence to use
the feature-limited and memory-restricted caOS FREE edition:
               **for evaluation and non-commercial development**
to compile, link, run and test your own programs against the precompiled caOS libraries on
the supported STM32 boards.

"Non-commercial development" means evaluation, study, teaching, personal projects and
prototypes that are not sold, licensed, rented or otherwise supplied to third parties for
a fee, and not used to provide paid services.

## 2. Reservation of rights

caOS, including the kernel, the drivers and their design, is and remains the property of
Angelo Coppi. No rights are granted other than those stated here. The kernel source code is
not part of this package and is not licensed to you.

## 3. Restrictions

You may **not**:

a. use the Software, or firmware built with it, in a commercial product or a production
   deployment without a separate commercial licence from the Licensor;
b. reverse engineer, disassemble or decompile the precompiled libraries, except where and
   to the extent the law expressly allows it notwithstanding a contractual prohibition
   (in Italy, articles 64-ter and 64-quater of Law 633/1941, implementing Directive
   2009/24/EC);
c. remove, disable or circumvent the edition limits, the integrity check or the
   anti-debug behaviour of the libraries, or attempt to do so;
d. redistribute modified libraries, or present modified libraries as caOS;
e. remove or alter any copyright, trademark or authorship notice.

You **may** redistribute this package unmodified, with this Agreement included, for
evaluation purposes and free of charge.

## 4. Your own code

Your application code — the files you write in `workspace/` and the examples you modify — is
yours. This Agreement governs the caOS Software, not your code.

## 5. Demonstration keys

The cryptographic keys shipped in the package (for example the bootloader signing key in
`workspace/app_2/` and the SSH keys of the examples) are **public demonstration keys**.
They provide no security. Before using the bootloader, SSH or any other signed or
authenticated service outside a test bench, replace them with your own keys as described
in the manual.

## 6. Third-party components

The `external/` folder contains third-party software (CMSIS, FatFs, Thread-Metric), each
under its own licence included alongside it. Those licences govern those components.

## 7. Trademarks

STM32, NUCLEO, ST-LINK and STM32CubeProgrammer are trademarks of STMicroelectronics; Arm and
Cortex are trademarks of Arm Limited; Raspberry Pi is a trademark of Raspberry Pi Ltd. caOS
is an independent work and is not affiliated with, sponsored by or endorsed by any of them.
This Agreement grants you no rights in those trademarks.

## 8. No warranty

The Software is provided free of charge, for evaluation. To the extent permitted by
applicable law, it is provided "AS IS", without warranty of any kind, express or implied,
including but not limited to the warranties of merchantability, fitness for a particular
purpose and non-infringement. The Software is not intended for use in safety-critical
systems.

## 9. Limitation of liability

To the extent permitted by applicable law, the Licensor shall not be liable for any damages
arising out of the use of or inability to use the Software. This limitation does not apply
to damages caused by wilful misconduct or gross negligence (article 1229 of the Italian
Civil Code), nor to any liability that cannot be excluded or limited under mandatory law,
including consumer protection law.

## 10. Privacy

The Software collects no personal data and sends nothing to the Licensor. The libraries
contain no telemetry and no network connection of their own: the network services run
only when your application enables them.

## 11. Termination

This Agreement terminates automatically if you breach any of its terms. On termination you
must stop using the Software and destroy your copies.

## 12. Governing law and jurisdiction

This Agreement is governed by the laws of Italy. Any dispute falls under the jurisdiction
of the Italian courts. If you are a consumer, the mandatory protections and the court of
your place of residence provided by the law of your country of residence remain
unaffected.

If any provision of this Agreement is held invalid, the remaining provisions stay in
force. This Agreement is written in English; any translation is for convenience only.

## 13. Contact

For a commercial licence or any question: Angelo Coppi, caos.rtos@protonmail.com.
