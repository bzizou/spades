#!/bin/bash

############################################################################
# Copyright (c) 2023-2024 SPAdes team
# Copyright (c) 2015-2022 Saint Petersburg State University
# Copyright (c) 2011-2014 Saint Petersburg Academic University
# All Rights Reserved
# See file LICENSE for details.
############################################################################

for i in ant00Agent ant00Agent_2 ant01agent ant02agent ant03agent ant04agent ant05agent ant06agent ant07agent ant13agent
do
        echo Restarting $i...
	./$i/bin/agent.sh stop
	./$i/bin/agent.sh start
        echo Done
done

