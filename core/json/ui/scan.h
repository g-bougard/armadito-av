/***

Copyright (C) 2015, 2016 Teclib'

This file is part of Armadito core.

Armadito core is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Armadito core is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Armadito core.  If not, see <http://www.gnu.org/licenses/>.

***/

#ifndef _SCAN_H_
#define _SCAN_H_

#include "jsonhandlerp.h"

enum a6o_json_status scan_response_cb(struct armadito *armadito, struct json_request *req, struct json_response *resp, void **request_data);

void scan_do_process_cb(struct armadito *armadito, void *request_data);

enum a6o_json_status scan_cancel_response_cb(struct armadito *armadito, struct json_request *req, struct json_response *resp, void **request_data);

#endif
