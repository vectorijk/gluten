/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package io.glutenproject.vectorized

import java.util.concurrent.TimeUnit

import org.apache.spark.TaskContext

import org.apache.spark.internal.Logging
import org.apache.spark.sql.execution.metric.SQLMetric
import org.apache.spark.sql.vectorized.ColumnarBatch

/**
 * An Iterator that insures that the batches [[ColumnarBatch]]s it iterates over are all closed
 * properly.
 */
class CloseableCHColumnBatchIterator(itr: Iterator[ColumnarBatch],
                                     pipelineTime: Option[SQLMetric] = None
                                    ) extends Iterator[ColumnarBatch] with Logging {
  var cb: ColumnarBatch = null

  private def closeCurrentBatch(): Unit = {
    if (cb != null) {
      if (cb.numCols() > 0) {
        val col = cb.column(0).asInstanceOf[CHColumnVector]
        val block = new CHNativeBlock(col.getBlockAddress)
        block.close();
      }
      cb.close()
      cb = null;
    }
  }

  TaskContext.get().addTaskCompletionListener[Unit] { _ =>
    closeCurrentBatch()
  }

  override def hasNext: Boolean = {
    val beforeTime = System.nanoTime()
    val res = itr.hasNext
    pipelineTime.map(t => t += TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - beforeTime))
    res
  }

  override def next(): ColumnarBatch = {
    val beforeTime = System.nanoTime()
    closeCurrentBatch()
    cb = itr.next()
    pipelineTime.map(t => t += TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - beforeTime))
    cb
  }
}
